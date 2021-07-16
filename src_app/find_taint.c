#include "taint.h"

// read-only constants

static const char *marks[] = {
	"None",			// 0	0
	"FctName",		// 1	1
	"Target",		// 2	2
	"Alloca",		// 4	3
	"Taintable",		// 8	4
	"DerivedFromTaint",	// 16	5
	"PropViaFct",		// 32	6
	"VulnerableTarget",	// 64	7
	"CorruptableSource",	// 128	8
	"ExternalSource",	// 256	9
	"DerivedFromExternal",	// 512	10
	"Stop"			// 1024	11
};

static const char *t_links[] = {
	 "[", "]", "(", ")", "{", "}"
};

static int setlimit;	// not modified after init

// globals

static int Into;
static int warnings;
static int banner;
static int ngets;
static int ngets_bad;
static int nfopen;

//	mset[0] stores a mark to indicate that scope was checked before
//	mset[1] stores marks indicating potentially dangerous targets
//	mset[2] stores reusable tags indicating fct definitions
//	mset[3] stores marks for vars set from external sources

// utility functions

static void
what_mark(const char *prefix, enum Tags t)
{	int i, j;
	int cnt = 0;

	for (i = j = 1; j < Stop; i++, j *= 2)
	{	if (t & j)
		{	printf("%s%s", cnt ? ", " : prefix, marks[i]);
			cnt++;
	}	}
}

void
show_bindings(const char *s, const Prim *a, const Prim *b, int cid)
{
	do_lock(cid);
	if (a)
	{	printf("Bind <%s>\t%s:%d: %s (%d)\tto ", s,
			a->fnm, a->lnr, a->txt, a->mark);
	}

	if (b)
	{	printf("%s:%d: %s ::",
			b->fnm, b->lnr, b->txt);
		what_mark(" ", b->mark);
		printf("\n");
	} else
	{	printf("--\n");
	}

	if (a
	&&  a->bound
	&&  a->bound != b)
	{	printf("\t\t[already bound to %s:%d: %s]\n",
			a->bound->fnm, a->bound->lnr, a->bound->txt);
	}
	do_unlock(cid);
}

static void
search_calls(Prim *inp, int cid)	// see if marked var inp.txt is used as a parameter in a fct call
{	Prim *r = inp;
	int cnt = 1;	// parameter nr

	if (r->mset[0] & SeenParam)	// handled before
	{	return;
	}
	r->mset[0] |= SeenParam;

	while (r && r->round >= inp->round)
	{	if (r->round == inp->round
		&&  strcmp(r->txt, ",") == 0)
		{	cnt++;
		}
		r = r->prv;
	}
	r = r?r->prv:0;	// should be the fct name

	if (!r
	|| strcmp(r->typ, "ident") != 0
	|| strcmp(r->txt, "main") == 0)
	{	return;
	}

	// if (verbose)
	// {	show_bindings("search_calls", r, inp->bound?inp->bound:inp, cid);
	// }
	// Remember
	//	fct name r->txt
	//	param name inp and nr (cnt)
	// there may be multiple entries for the same call

	param_is_tainted(r, cnt, inp, cid);	// store and check use of the param in this fct later
}

static void
save_set_range(Prim *from, Prim *upto, int cid)
{	Prim *mycur = from;
	int into = Into;	// get a local copy

	while (mycur && mycur->seq <= upto->seq)
	{	mycur->mset[into] = mycur->mark;
		mycur->mbnd[into] = mycur->bound;
		mycur->mark  = 0;
		mycur->bound = 0;
		mycur = mycur->nxt;
	}
}

static void *
save_set_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	save_set_range(from, upto, *i);

	return NULL;
}

static void
save_set(const int into)
{
	Into = into;
	run_threads(save_set_run);
}

static Prim *
move_to(Prim *p, const char *s)
{
	while (p && strcmp(p->txt, s) != 0)
	{	p = p->nxt;
	}
	return p;
}

static void
check_var(Prim *r, Prim *x, int cid)	// mark later uses of r->txt within scope level, DerivedFromExternal
{	Prim *w = r;		// starting at token x
	int level = x?x->curly:0;

// printf("%d: check_var %s - forward scan <%d> ((%d::%s))\n", r->lnr, r->txt, r->mark, x->lnr, x->txt);

	if (x
	&&  r->seq < x->seq)
	{	// r is a formal parameter
		// and the block to search starts at x
		r = x;
	} else
	{	r = r->nxt;
	}

	while (r && r->curly >= level)
	{	if (strcmp(r->txt, w->txt) == 0)
		{	r->mark |= DerivedFromExternal;
			if (verbose)
			{	show_bindings("check_var", r, w, cid);
			}
			r->bound = w; // point to place where an external source was tagged
			if (verbose > 1)
			{	do_lock(cid);
				printf("%d: %s:%d: marked new occurrence of %s (+%d -> %d)\n",
					cid, r->fnm, r->lnr, r->txt, DerivedFromExternal, r->mark);
				do_unlock(cid);
		}	}
		r = r->nxt;
	}
}

static void
trace_back(const char *prefix, const Prim *p, const int a)
{	Prim *e, *f = (Prim *) 0;
	int cnt=10, first=1;

	if (!p)
	{	return;
	}
	for (e = p->mbnd[a]; cnt > 0 && e; cnt--, e = e->mbnd[a])
	{	if (e != f
		&& (e->mset[a] != Target || verbose))
		{	if (first)
			{	first = 0;
				printf("%s\n", prefix);
			}
			printf("\t\t>%s:%d: %s (", e->fnm, e->lnr, e->txt);
			what_mark("", e->mset[a]);
			what_mark("\t", e->mset[(a==3)?1:3]);	// 1->3, 3->1
			printf(")\n");
			if (!f) { f = e; } // remember the first
	}	}
}

static void
add_details(Prim *p, const int a, Prim *q, const int b)
{	//   prv		 nxt
	if (p->mset[a])
	{	what_mark("\tT:", p->mset[a]);
	}
	if (q->mset[b])
	{	what_mark("\tE:", q->mset[b]);
	}
	printf("\n");
	trace_back("\t  (taints)", p, a);
	trace_back("\t  (external)", q, b);
}

// the main steps

static int
step1_find_sources(void)	// external sources of potentially malicious data
{	Prim *q;
	int pos, cnt = 0, isfd;
	unsigned int bad;
	char *z, *r;

	//  argv
	//  scanf(     "...", arg, ...)
	// fscanf(fd,  "...", arg, ...)
	// sscanf(str, "...", arg, ...) <- handled with assignments
	// fgets(str, size, fd)
	//  gets(str)
	// getline(buf, size, fd)
	// getdelim(buf, size, delim, fd)

	for (cur = prim; cur; cur = cur->nxt)	// step1: mark potentially tainted sources
	{
		if (strcmp(cur->txt, "argv") == 0)	// always marked, whether in main or not
		{	cur->mark |= ExternalSource;
			cnt++;
			if (verbose > 1)
			{	printf("%s:%d: marked '%s' (+%d)\n",
					cur->fnm, cur->lnr, cur->txt, ExternalSource);
			}
			continue;
		}

		if (strcmp(cur->txt, "scanf") == 0)		// mark args below
		{	isfd = 0;
		} else if (strcmp(cur->txt, "fscanf") == 0)	// mark args below
		{	isfd = 1;
		} else if (strcmp(cur->txt, "gets") == 0)	// warn right away
		{	q = cur->nxt;
			if (q && q->txt[0] != '(')		// not a fct call
			{	continue;
			}
			isfd = 0;
			if (verbose || !no_match || !no_display)	// no -terse argument
			{	printf(" %3d: %s:%d: warning: unbounded call to gets()\n",
					++warnings, cur->fnm, cur->lnr);
			}
			ngets++;
		//	q = move_to(cur->nxt, "(");
			if (!q)
			{	continue;
			}
			q = q->nxt;
			if (strcmp(q->typ, "ident") == 0)
			{	q->mark |= ExternalSource;
				cnt++;
				if (verbose > 1)
				{	printf("%s:%d: marked gets arg '%s' (+%d)\n",
						q->fnm, q->lnr, q->txt, ExternalSource);
			}	}
			continue;
		} else if (strcmp(cur->txt, "fgets") == 0
		       ||  strcmp(cur->txt, "getline") == 0
		       ||  strcmp(cur->txt, "getdelim") == 0)	// mark first arg
		{	q = cur->nxt;
			if (q && q->txt[0] != '(')	// not a fct call
			{	continue;
			}
		//	q = move_to(cur->nxt, "(");
			if (!q)
			{	continue;
			}
			q = q->nxt;
			if (q && strcmp(q->typ, "ident") == 0)
			{	q->mark |= ExternalSource;
			}
			continue;
		} else
		{	continue;
		}

		// *scanf calls, mark corruptable args
		q = cur->nxt;
		if (!q || strcmp(q->txt, "(") != 0)
		{	continue;
		}
		if (isfd)	// move passed the fd or str field
		{	while (q && strcmp(q->txt, ",") != 0)
			{	q = q->nxt;
			}
			if (!q)
			{	continue;
			}
			q = q->nxt;
		}

		bad = pos = 0;
		while (q && strcmp(q->txt, ",") != 0)	// move to 2nd arg, skipping format string
		{	char *u = q->txt;

			while ((z = strstr(u, "%s")) != NULL)	// checking for unbounded %s
			{	r = u;
				while (r <= z)
				{	if (*r == '%'
					&&  *(r+1) != '%')
					{	pos++;
					}
					r++;
				}
				if (pos < 8*sizeof(unsigned int))
				{	bad |= 1<<pos;	// allows up to 64 params
				}
				u = z+2;
			}
			q = q->nxt;
		}
		if (!bad || !q)				// at least one unbounded %s seen
		{	continue;
		}

		q = q->nxt;
		pos = 1;
		while (strcmp(q->txt, ")") != 0
		&&     q->round > cur->round)	// all args
		{	if (strcmp(q->txt, ",") == 0)
			{	pos++;
			} else if (strcmp(q->typ, "ident") == 0
			       &&  (bad & (1<<pos)))
			{	q->mark |= ExternalSource;	// mark the corresponding args
				cnt++;
				if (verbose > 1)
				{	printf("%s:%d: marked scanf arg '%s' (+%d)\n",
						q->fnm, q->lnr, q->txt, ExternalSource);
			}	}
			q = q->nxt;
		}
	}
	if (verbose && cnt > 0)
	{	printf("marked %d vars as potential external taint source%s (+%d)\n",
			cnt, cnt>1?"s":"", ExternalSource);
	}
	return cnt;
}

static int
handle_sscanf(Prim *from, int cid)
{	Prim *mycur, *q = from->nxt;
	char *r, *z, *u;
	unsigned int bad = 0, pos = 0;
	int cnt = 0;

	mycur = from;

	if (strcmp(q->txt, "(") != 0)
	{	return 0;
	}
	q = q->nxt;
	if (q->mark == 0)	// unless first arg tainted
	{	return 0;
	}
	q = q->nxt;
	if (strcmp(q->txt, ",") != 0)
	{	return 0;
	}
	q = q->nxt;	// the format string
	if (strstr(q->txt, "%s") == NULL)
	{	return 0;
	}

	u = q->txt;	// handle: sscanf(tainted, "....", propagating to %s args
	while ((z = strstr(u, "%s")) != NULL)
	{	r = u;	// find arg position of %s
		while (r <= z)
		{	if (*r == '%'
			&&  *(r+1) != '%')
			{	pos++;
			}
			r++;
		}
		if (pos < 8*sizeof(unsigned int))
		{	bad |= 1<<pos;	// up to 64 args
		}
		u = z+2; // skip over %s, and search for more
	}
	q = q->nxt;	 // , after format string
	if (!bad || !q)
	{	return 0;
	}

	pos = 1;
	while (strcmp(q->txt, ")") != 0
	&&     q->round > mycur->round)	// all args
	{	if (strcmp(q->txt, ",") == 0)
		{	pos++;
		} else  if (strcmp(q->typ, "ident") == 0
			&& (bad & (1<<pos))
			&& !(q->mark & DerivedFromExternal))
		{	q->mark |= DerivedFromExternal;
			cnt++;
			if (verbose > 1)
			{	do_lock(cid);
				printf("%d: %s:%d: marked sscanf arg '%s' (+%d)\n",
					cid, q->fnm, q->lnr, q->txt, DerivedFromExternal);
				do_unlock(cid);
		}	}
		q = q->nxt;
	}

	return cnt;
}

static void
step2a_pre_scope_range(Prim *from, Prim *upto, int cid)	// propagate external source marks
{	Prim *mycur, *q;
	int cnt = 0; // step2a: prop of tainted thru assignments thru '=' or sscanf

	for (mycur = from; mycur && mycur->seq <= upto->seq; mycur = mycur->nxt)
	{
		if (strcmp(mycur->txt, "sscanf") == 0)
		{	cnt += handle_sscanf(mycur, cid);
			continue;
		}

		if (mycur->mark == 0)
		{	continue;
		}
		q = mycur->prv;

		if (q && strchr(q->txt, '=')
		&& (q->txt[1] == '\0'
		|| (q->txt[0] != '='
		&&  q->txt[0] != '>'
		&&  q->txt[0] != '<'
		&&  q->txt[0] != '!')))	// tainted var assigned to something
		{	q = q->prv;	// propagate mark to lhs

			if (strcmp(q->txt, "]") == 0) // skip over possible array index
			{	q = q->jmp;
				q = q?q->prv:q;
			}

			if (q
			&&  strcmp(q->typ, "ident") == 0
			&&  !(q->mark & DerivedFromExternal))
			{	q->mark |= DerivedFromExternal;
				if (verbose > 1)
				{	do_lock(cid);
					printf("\t%s:%d: prescope propagated %s (+%d)\n",
						q->fnm, q->lnr, q->txt, DerivedFromExternal);
					do_unlock(cid);
				}
				cnt++;
		}	}
	}
	tokrange[cid]->param = cnt;
	if (verbose > 1)
	{	do_lock(cid);
		printf("%d: step2a_pre_scope tries to return %d\n", cid, cnt);
		do_unlock(cid);
	}
}

static void *
step2a_pre_scope_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	step2a_pre_scope_range(from, upto, *i);

	return NULL;
}

static int
step2a_pre_scope(void)
{	int cnt;

	cnt = run_threads(step2a_pre_scope_run);
	if (verbose > 1)
	{	printf("step2a_pre_scope returns %d\n", cnt);
	}
	return cnt;
}

static int
likely_decl(const Prim *p)
{	Prim *q;

	if (!p)
	{	return 0;
	}
	q = p->prv;

	while (q && strcmp(q->txt, "*") == 0)
	{	q = q->prv;
	}

	if (q
	&&  (strcmp(q->typ, "type") == 0
	||   strcmp(q->txt, ",")    == 0))
	{	return 1;
	}

	return 0;
}

static void
step2b_check_scope_range(Prim *from, Prim *upto, int cid)	// ExternalSource|DerivedFromExternal, DerivedFromExternal
{	Prim *q, *mycur;

	// find declaration for marked external sources, so that
	// we can check all occurrences within the same scope

	mycur = from;
	while (mycur && mycur->seq <= upto->seq)	// step2b: use of tainted within same scope
	{	if (!(mycur->mark & (ExternalSource|DerivedFromExternal | DerivedFromExternal))
		||   (mycur->mset[0] & SeenVar))
		{	mycur = mycur->nxt;
			continue;
		}
		mycur->mset[0] |= SeenVar;	// only once per marked variable

		// search backwards to likely point of declaration
		for (q = mycur; q && !likely_decl(q); )
		{	q = q->prv;
			while (q
			&&     (q->curly > 0 || q->round > 0)	// stick to locals & params
			&&     strcmp(q->txt, mycur->txt) != 0)	// find the same ident name
			{	q = q->prv;
			}
			if (!q
			||  (q->curly <= 0 && q->round <= 0))	// probably not local
			{	q = 0;
		}	}
		if (!q)
		{	mycur = mycur->nxt;
			continue;
		}

		// found the likely point of decl, which could be a formal param
		// q->txt is the name of the variable, matching mycur->txt
		if (verbose > 1)
		{	do_lock(cid);
			printf("%d: %s:%d: %s likely declared here (level %d::%d)\n",
				cid, q->fnm, q->lnr, mycur->txt, q->curly, q->round);
			do_unlock(cid);
		}
		if (q->mset[0] & SeenDecl) // checked before for this var
		{	mycur = mycur->nxt;
			continue;
		}
		q->mset[0] |= SeenDecl;

		if (q->curly == 0)		// must be a formal parameter
		{	if (q->round != 1)	// nope, means we dont know
			{	if (verbose>1)
				{	do_lock(cid);
					printf("%d: %s:%d: '%s' untracked global (declared at %s:%d)\n",
						cid, mycur->fnm, mycur->lnr, mycur->txt, q->fnm, q->lnr);
					do_unlock(cid);
				}
				mycur = mycur->nxt;
				continue; 	// likely global, give up
			}
			q = move_to(q, "{");	// scope of param is the fct body
		} else
		{	Prim *r = q;
			while (q->curly == r->curly)
			{	q = q->prv;	// local decl: find start of fct body
		}	}

		if (!q)
		{	mycur = mycur->nxt;
			continue;		// give up
		}

		// located the block level where mycur->txt is declared:
		// now we must search this scope to mark all new
		// occurences that appear *after* mycur itself upto the
		// end of this level of scope

		if (0)
		{	do_lock(cid);
			printf("%d: check_var %s:%d '%s' level %d <<%d %d>>\n",
				cid, mycur->fnm, mycur->lnr, mycur->txt, q->nxt->curly,
				mycur->seq, q->nxt->seq);
			do_unlock(cid);
		}
		check_var(mycur, q->nxt, cid);	// mark DerivedFromExternal
		mycur = mycur->nxt;
	}
}

static void *
step2b_check_scope_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	step2b_check_scope_range(from, upto, *i);

	return NULL;
}

static void
step2b_check_scope(void)
{
	run_threads(step2b_check_scope_run);
}

static void
step2c_part1_range(Prim *from, Prim *upto, int cid)
{	Prim *mycur;

	mycur = from;
	while (mycur && mycur->seq <= upto->seq)	// step2c_6: prop of tainted thru fct calls
	{	if (mycur->mark != 0
		&&  mycur->round > 0
		&&  mycur->curly > 0)
		{	search_calls(mycur, cid);
			// check if marked var is used as a param in a fct call
			// and remember these params in param_is_tainted
			// which is used below to mark vars derived from these
		}
		mycur = mycur->nxt;
	}
}

static void *
step2c_part1_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	step2c_part1_range(from, upto, *i);

	return NULL;
}

static void
step2c_part1(void)
{
	run_threads(step2c_part1_run);
}

#if 1
static void
step2c_part2_range(Prim *from, Prim *upto, int cid)
{	Prim *mycur = from;
	Prim *q;
	int cnt = 0;

	while (mycur && mycur->seq <= upto->seq)	// step2c_6: prop of tainted thru fct params and returns
	{	if (mycur->mset[2])		// for each function
		{	q = mycur;		  // save
			cnt += is_param_tainted(mycur, setlimit, cid);  // mark uses of tainted params PropViaFct
			mycur = q;		  // restore
			search_returns(mycur, cid); // used in next scan below
		}
		mycur = mycur->nxt;
	}
	tokrange[cid]->param = cnt;
}

static void *
step2c_part2_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	step2c_part2_range(from, upto, *i);

	return NULL;
}

static int
step2c_part2(void)
{	int cnt;

	cnt = run_threads(step2c_part2_run);
	return cnt;
}

#else
static int
step2c_part2(void)
{	Prim *q;
	int cnt = 0;

	for (cur = prim; cur; cur = cur->nxt)	// step2c_6: prop of tainted thru fct params and returns
	{	if (cur->mset[2])		// for each function
		{	q = cur;		  // save
			cnt += is_param_tainted(cur, setlimit, 0);  // mark uses of tainted params PropViaFct
			cur = q;		  // restore
			search_returns(cur, 0); // used in next scan below
	}	}
	return cnt;
}
#endif

static void
step2c_part3_range(Prim *from, Prim *upto, int cid)
{	Prim *mycur, *r;
	int cnt = 0;

	// step2c_6: use of tainted returns from fct calls
	for (mycur = from; mycur && mycur->seq <= upto->seq; mycur = mycur->nxt)
	{	if (strcmp(mycur->txt, "(") == 0
		&&  mycur->curly > 0
		&&  mycur->prv
		&&  strcmp(mycur->prv->typ, "ident") == 0
		&&  is_return_tainted(mycur->prv->txt, cid))
		{	// possible fct call returning tainted result
			r = mycur->prv->prv;	// token before the call
			if (strchr(r->txt, '=') != NULL) // used in assignment?
			{	r = r->prv;
				if (strcmp(r->txt, "]") == 0
				&&  r->jmp)
				{	r = r->jmp->prv;
				}
				if (r
				&&  strcmp(r->typ, "ident") == 0
				&&  r->mark == 0)
				{	r->mark |= PropViaFct;	// mark it
					if (verbose)
					{	show_bindings("iterate", r, mycur->prv, cid);
					}
					if (!r->bound)
					{	r->bound = mycur->prv; // origin, fct nm
					}
					cnt++;
	}	}	}	}
	tokrange[cid]->param = cnt;
}

static void *
step2c_part3_run(void *arg)
{	int *i = (int *) arg;
	Prim *from, *upto;

	from = tokrange[*i]->from;
	upto = tokrange[*i]->upto;

	step2c_part3_range(from, upto, *i);

	return NULL;
}

static int
step2c_part3(void)
{	int cnt;

	cnt = run_threads(step2c_part3_run);
	return cnt;
}

static int
step2c_step6_iterate(void)	// PropViaFct
{	int cnt;

	// check propagation of the marked variables (vulnerable targets)
	// through fct calls or via return statements

	       step2c_part1();	// parallel
	cnt  = step2c_part2();	// parallel
	cnt += step2c_part3();	// parallel

	if (verbose)
	{	printf("iterate (%d) returns %d\n", PropViaFct, cnt);
	}

	return cnt;
}

static void
step3_step7_check_uses(const int v)	// used as args of: memcpy, strcpy, sprintf, or fopen
{	Prim *q;			// VulnerableTarget or CorruptableSource
	int cnt = 0;
	int nr_commas;

	for (cur = prim; cur; cur = cur->nxt)	// step3_7: check suspect uses of tainted data
	{
		if (cur->round == 0
		|| cur->mark == 0
//		||  (v == VulnerableTarget  && cur->mset[1] == 0)
//		||  (v == CorruptableSource && cur->mset[3] == 0)
		|| (cur->mark & v))
		{	continue;
		}
		// was marked before, but not with v
		if (verbose > 1)
		{	printf("%s:%d: Check Uses, mark %d, txt %s round: %d\n",
				cur->fnm, cur->lnr, v, cur->txt, cur->round);
		}

		q = cur;
		nr_commas = 0;
		while (q->round >= cur->round)
		{	if (q->round == cur->round
			&&  strcmp(q->txt, ",") == 0)
			{	nr_commas++;	// nr of actual param
			}
			q = q->prv;
		}
		q = q->prv;	// fct name
		if (verbose > 1)
		{	printf("\t\tfctname? '%s' round %d nrcommas %d (v=%d cm %d)\n",
				q->txt, q->round, nr_commas, v, cur->mark);
		}

		if (strcmp(q->txt, "sizeof") == 0)
		{	continue;
		}
		if ((strcmp(q->txt, "fopen") == 0	// make sure the fd doesn't have external source
		||   strcmp(q->txt, "freopen") == 0)
		&&  nr_commas == 0			// first arg: pathname
		&&  (cur->mark & (ExternalSource | DerivedFromExternal)))
		{	if (verbose || !no_display || !no_match)		// no -terse argument was given
			{	cnt++;
				printf(" %3d: %s:%d: warning: contents of '%s' in call to fopen() possibly tainted",
					++warnings, cur->fnm, cur->lnr, cur->txt);
				what_mark(": ", cur->mark);
				printf("\n");
			}
			nfopen++;
			continue;
		}

		// in the printf family, if a %n format specifier occurs
		// then the corresponding argument ptr is vulnerable and
		// should not be a corruptable: warn if a marked arg links to %n

		if (strstr(q->txt, "printf") != NULL)
		{	Prim *w;
			char *qtr;
			int nr;		// counts nr of format specifiers before %n
			int correction=0;

			w = q->nxt;				// (
			w = w->nxt;				// the format string for printf
			if (strcmp(w->typ, "ident") == 0)	// sprintf/fprintf/snprintf
			{	w = w->nxt;			// skip ,
				w = w->nxt;			// the format string
				correction = 1;			// remember the extra arg
			}

			if (strcmp(q->txt, "snprintf") == 0)
			{	correction = 2;			// account for size arg
				while (strcmp(w->txt, ",") != 0)
				{	w = w->nxt;		// skip over size arg
				}
				w = w->nxt;
			}

			// scan the format string for conversions specifiers
			for (qtr = w->txt, nr = 1; ; qtr++, nr++)
			{	while (*qtr != '\0' && *qtr != '%')
				{	qtr++;
				}
				if (*qtr == '\0'
				|| *(qtr+1) == '\0')
				{	break;	// checked all
				}
				qtr++;	// char following %

				switch (*qtr) {
				case 'n':
					if (0)
					{	printf("%d\tsaw %%n, nr %d nr_commas %d correction %d (%s)\n",
							cur->lnr, nr, nr_commas, correction, cur->txt);
					}
					// the nr-th format conversion is %n
					// check if its that the one that was marked
					if (nr == nr_commas - correction)
					{	printf(" %3d: %s:%d: warning: %s() ",
							++warnings, cur->fnm, cur->lnr, q->txt);
						printf("uses '%%n' with corruptable arg '%s'\n", cur->txt);
					}
					break;
				case '%':	// a literal %%
					nr--;
					break;
				default:
					break;
		}	}	}

		// check other fcts, including sprintf
		if (strcmp(q->txt, "memcpy") == 0
		||  strcmp(q->txt, "strcat") == 0
		||  strcmp(q->txt, "strcpy") == 0
		||  strcmp(q->txt, "sprintf") == 0
		||  (v == CorruptableSource && strcmp(q->txt, "gets") == 0))
		{
			if (((v == VulnerableTarget  && nr_commas == 0)
			||   (v == CorruptableSource && nr_commas  > 0))
			&&  !(cur->mark & v))	// not already marked
			{	cur->mark |= v;
				if (verbose)
				{	show_bindings("check_uses", cur, q, 0);
				}
				if (!cur->bound)
				{	cur->bound = q;	// origin
				} // else printf("6 does this happen?\n");
				cnt++;
				if (verbose > 1)
				{	printf("%s:%d: %s (mark |= %d) used as target in call of %s()\n",
					cur->fnm, cur->lnr, cur->txt, v, q->txt);
	}	}	}	}

	if (verbose)
	{	printf("%d of the potential targets used in suspicious calls (+%d)\n",
			cnt, v);
	}
}

static int
step4_find_targets(void)
{	Prim *q, *r;
	int cnt = 0;

	for (cur = prim; cur; cur = cur?cur->nxt:0)	// step4: mark potentially corruptable targets
	{	// dont look inside struct declarations
		if (cur->curly > 0	// we're considering local decls only
		&&  (MATCH("struct")
		||   MATCH("union")))
		{	// may be a ref, not a def
			// check if we get to a ';' before we see a '{'
			while (!MATCH("{") && cobra_nxt())
			{	if (MATCH(";"))		// not a def, dont skip
				{	break;
			}	}
			if (!cur)
			{	break;
			}
			if (!MATCH(";") && cur->jmp)
			{	cur = cur->jmp;
			}
			continue;
		}
		if (MATCH("static"))	// not a stack var
		{	if (!cobra_nxt())	// skips the following type
			{	break;
			}
			continue;
		}
		if (MATCH("alloca"))	// just 2 matches in linux3.0 sources
		{	q = cur;
			cur = cur->prv;
			while (!MATCH("=")
			&&     !MATCH(";")
			&&     !TYPE("ident")
			&&     cur->curly > 0)
			{	cur = cur->prv;
			}
			if (!MATCH("="))
			{	cur = q;
				continue;
			}
			cur = cur->prv;	
			if (!TYPE("ident"))
			{	cur = q;
				continue;
			}
			cur->mark |= Alloca;
			cnt++;
			if (verbose > 1)
			{	printf("possibly taintable (alloca): %s:%d: %s\n",
					cur->fnm, cur->lnr, cur->txt);
			}
			cur = q;	// restore
			continue;
		}
		if (TYPE("type")
		&&  cur->curly > 0	// local to fct
		&&  cur->round == 0)	// not a type cast
		{	q = cur;
findmore:
			while (!TYPE("ident")
			&&     !MATCH(";")
			&&     cur->curly == q->curly)
			{	if (MATCH("="))	// skip initializers
				{	while (!MATCH(",") && !MATCH(";"))
					{	cur = cur->nxt;
					}
					if (MATCH(";"))
					{	break;
				}	}
				cur = cur->nxt;
			}
			if (TYPE("ident")
			&&  cur->round == 0		// not a fct arg
			&&  cur->curly == q->curly)	// still in same block
			{	r = cur;
				cur = cur->nxt;
				if (MATCH("["))		// arrays are vulnerable
				{	r->mark |= Target;
					if (verbose > 1)
					{	printf("possibly taintable: %s:%d: %s\n",
							r->fnm, r->lnr, r->txt);
					}
					cnt++;
				}
				goto findmore;
			}
			cur = q;	// restore
	}	}

	if (verbose && cnt > 0)
	{	printf("found %d potentially taintable sources (marked with %d)\n", cnt, Target);
	}

	return cnt;
}

static void
step5_find_taints(void)		// completes Step 1 from taint_track.cobra script
{	Prim *q, *r, *s;	// marks UseOfTaint or DerivedFromTaint
	int cnt = 0;

	for (cur = prim; cur; cur = cur->nxt)	// step5: find additonal uses of marked items, forward search
	{	if (cur->mark)		// a target, search to the end of the fct
		{	q = cur;	// check where else the name occurs,
					// and if it is assigned to another variable
			if (strcmp(q->prv->txt, ".")  == 0
			||  strcmp(q->prv->txt, "->") == 0)
			{	continue;	// not top-level name
			}
		
			cur = cur->nxt;
			while (cur->curly > 0)		// up to end of fct
			{	if (!MATCH(q->txt)	// not same ident
				||  strcmp(cur->prv->txt, ".") == 0
				||  strcmp(cur->prv->txt, "->") == 0)	// not toplevel name
				{	cur = cur->nxt;
					continue;
				}
				if (!(cur->mark & UseOfTaint))
				{	if (cur->mset[3])		// was it marked ExternalSource before?
					{	char lbuf[2048];
						int len = 0, noprint = 1;	// by default, don't print these

						sprintf(lbuf, " %3d: %s:%d: '%s' in ", ++warnings,
							cur->fnm, cur->lnr, cur->txt);
						r = cur;
						while (r->round > 0 && r->round == cur->round)
						{	r = r->prv;
						}
						r = r->prv;	// token before ( ... ) if enclosed
						r = r->prv;	// undo first r->nxt
						len = strlen(lbuf);
						do {	r = r->nxt;
							if (len + strlen(r->txt) + 1 < sizeof(lbuf))
							{	strcat(lbuf, r->txt);
							}
							if (strcmp(r->txt, "gets") == 0)
							{	ngets_bad++;
								if (verbose || !no_display || !no_match)
								{	noprint = 0; // print if not terse
							}	}
						} while (r->seq != cur->nxt->seq);

						if (noprint == 0)			// not gets()
						{	if (!banner)
							{	printf("=== Potentially dangerous assignments:\n");
								banner = 1;
							}
							printf("%s", lbuf);	// print warning
							if (r->txt[0] == ','
							||  r->txt[0] == '[')
							{	printf("...");
							}
							cur->mset[1] |= UseOfTaint; // for add_details XXX
							cur->mbnd[1] = cur->bound;
							add_details(cur, 1, cur, 3);
						} else
						{	warnings--;		// no warning issued, undo increment
						}
						cur->mark |= UseOfTaint;	// use of the taintable var
						break; // no need to tag subsequent uses of the same var
					}
					cur->mark |= UseOfTaint;	// use of the taintable var
					if (verbose)
					{	show_bindings("find_taints1", cur, q, 0);
					}
					if (!cur->bound)
					{	cur->bound = q;	// remember origin
					} // else printf("1 does this happen?\n");
				} else
				{	break;	// already searched from here for q->txt
				}

				if (verbose > 1)
				{	printf("%s:%d: marked place where %s is used (+%d -> %d [%d::%d])\n",
						cur->fnm, cur->lnr, q->txt, UseOfTaint,
						cur->mark, cur->mset[1], cur->mset[3]);
				}

				r = cur->prv;
				s = cur->nxt;
				if (strcmp(s->txt, "[") == 0		// array element
				||  strchr(s->txt, (int) '=') != 0)	// q followed by =
				{	cur = cur->nxt;
					continue;			// if =, q is overwritten
				}
				if (strcmp(r->txt, "&") == 0)		// q's address is taken
				{	r = r->prv;
				}
				if (strcmp(r->txt, "=") == 0)		// q used in assignment
				{	r = r->prv;
					if (strcmp(r->txt, "]") == 0)
					{	r = r->jmp;
						r = r?r->prv:r;
					}
					if (strcmp(r->typ, "ident") == 0
					&&  !(r->mark & DerivedFromTaint))
					{	r->mark |= DerivedFromTaint;	// newly tainted
						if (verbose)
						{	show_bindings("find_taints2", r, q, 0);
						}
						if (!r->bound)
						{	r->bound = q;	// remember origin
						} // else printf("2 does this happen?\n");

						cnt++;
						if (verbose > 1)
						{	printf("%s:%d '%s' tainted by asignment from taintable '%s'\n",
								r->fnm, r->lnr, r->txt, s->prv->txt);
				}	}	}
			// doesnt catch less likely assignments, like ptr = offset + buf
			// not yet: n.q = buf n->q = buf
				cur = cur->nxt;
			}
			cur = q;	// restore cur
	}	}

	if (verbose && cnt > 0)
	{	printf("found %d propagated marks (%d)\n", cnt, UseOfTaint);
	}
}

static void
issue_warning(Prim *q, const int a, const int b)
{	int none = 1;
	Prim *p;

	if (!banner)
	{	printf("=== Potentially dangerous assignments:\n");
		banner = 1;
	}
	printf(" %3d: %s:%d: in call to '%s'", ++warnings, cur->fnm, cur->lnr, cur->txt);
	printf(" marked arguments:");
	for (p = q->nxt; p && p->round > cur->round; p = p->nxt)
	{	if (strcmp(p->txt, ",") == 0)
		{	continue;
		}
		if (p->mset[a] + p->mset[b] == 0)
		{	continue; // skip this arg
		}
		if (none)
		{	printf("\n");
			none = 0;
		}
		printf("\t%s", p->txt);
		add_details(p, a, p, b);
	}
	if (none)
	{	printf("\n");
	}
}

static void
step8_check_contamination(const int a, const int b)
{	Prim *p, *q;
	int cnt, nrcommas, nrdots, level;

	// a) dangerous assignments
	for (cur = prim; cur; cur = cur->nxt)	// step8: assignments & calls
	{	if (strcmp(cur->typ, "ident") != 0
		|| (strcmp(cur->txt, "sprintf") != 0	// >1
		&&  strcmp(cur->txt, "snprintf") != 0	// >1
		&&  strcmp(cur->txt, "strcpy") != 0	// 2nd
		&&  strcmp(cur->txt, "strcat") != 0	// 2nd
		&&  strcmp(cur->txt, "strncpy") != 0	// 2nd
		&&  strcmp(cur->txt, "strncat") != 0	// 2nd
		&&  strcmp(cur->txt, "memcpy") != 0))	// 2nd
		{	continue;
		}

		// the first arg should have mset[a] != 0 -- ie a corruptable target
		// some the remaining args should have mset[b] != 0 -- ie an external source

		p = q = move_to(cur, "(");
		cnt = nrcommas = nrdots = 0;
		level = (p && p->nxt)?p->nxt->round:0;
		do {
			p = p->nxt;
			if (strcmp(p->txt, ",") == 0)
			{	nrdots = 0;
				nrcommas++;
			} else if (strcmp(p->txt, ".") == 0
			       ||  strcmp(p->txt, "->") == 0)
			{	nrdots++;
			} else if (nrdots == 0 && p->bracket == 0 && p->round == level)
			{	if (p->mset[a] != 0 && nrcommas == 0)		// Taintable
				{	cnt |= 1;
				}
				if (p->mset[b] != 0 && nrcommas != 0)	// External
				{
					if (strstr(cur->txt, "printf") != NULL	// sprintf or snprintf: any arg
					||  nrcommas == 1)			// the others: 2nd arg only
					{	cnt |= 2;
			}	}	}
		} while (cnt < 3 && p && p->nxt && p->round > cur->round); // arg list
		if (cnt == 3)
		{	issue_warning(q, a, b); // q is start of arg list
		}
	}
}

static void
taint_usage(void)
{
	printf("find_taint: unrecognized -- option(s): '%s'\n", backend);
	printf("valid options are:\n");
	printf("	--sanity	start with a check for missing links in input token-stream\n");
	printf("	--sequential	disable parallel processing in backend (overruling -N settings)\n");
	printf("	--limitN	with N a number, limit distance from fct call to fct def to Nk tokens\n");
	printf("	--exit		immediately exit after startup phase in front end is completed\n");
}

static void
sanity_check(void)
{	int B[6] = { 0, 0, 0 };
	int L[6] = { 0, 0, 0 };
	int cnt = 0;
	int i;

	for (cur = prim; cur; cur = cur?cur->nxt:0)
	{	cnt++;
		for (i = 0; i < 6; i++)
		{	if (strcmp(cur->txt, t_links[i]) == 0)
			{	if (cur->jmp == 0)
				{	B[i]++;
					if (i==5)
					{	printf("missing link on '%s': %s:%d\n",
							t_links[i], cur->fnm, cur->lnr);
					}
				} else if (strcmp(cur->fnm, cur->jmp->fnm) != 0)
				{	L[i]++;
					printf("cross-file link on '%s': %s:%d -> %s:%d (seq: %d -> %d\n",
						t_links[i],
						cur->fnm, cur->lnr,
						cur->jmp->fnm, cur->jmp->lnr,
						cur->seq, cur->jmp->seq);
	}	}	}	}

	printf("%d tokens\n", cnt);
	printf("tokens without link:\n");
	for (i = 0; i < 6; i++)
	{	printf("\t'%s'=%d", t_links[i], B[i]);
	}
	printf("\ncross-file links:\n");
	for (i = 0; i < 6; i++)
	{	printf("\t'%s'=%d", t_links[i], L[i]);
	}
	printf("\n");
}

static int
valid_args(void)
{	char *arg;
	int valid = 0;

	if ((arg = strstr(backend, "limit")) != NULL)
	{	arg = &arg[5];
		if (isdigit((int) *arg))
		{	setlimit = atoi(arg);
			printf("limiting distance from fct call to fct def to %d,000 tokens\n", setlimit);
			valid++;
	}	}
	if (strstr(backend, "sequential") != NULL)
	{	Ncore = 1;	// option --sequential, overrides -N from front-end
		printf("disabled parallel processing in backend\n");
		valid++;
	}
	if (strstr(backend, "sanity") != NULL)
	{	sanity_check();
		valid++;
	}
	if (strstr(backend, "exit") != NULL)	// to measure time needed for startup alone
	{	exit(0);
	}

	if (!valid)	// not a great test
	{	taint_usage();
	}

	return valid;
}

void
cobra_main(void)
{	int cnt = 0;

	if (strlen(backend) > 0
	&&  !valid_args())
	{	return;
	}

	set_multi();
	ini_timers();
	taint_init();

	mark_fcts();	// sequential
	save_set(2);	// marking function definitions

	// phase 1 of 3
	// 	mark data that could originate from external inputs
	// 	after locating the initial external sources, iterate
	//	to find
	// 	 1. additional uses within the same scope
	// 	 2. assignments to other variables (leading back to 1)
	// 	 3. parameters to function calls (leading back to 1 and 2)
	//
	// 	 the Phase2 search for vulnerable (stack-based) targets is
	// 	 simpler since they can only reasonably propagate through
	//	 pointers to those targets (in assignments and fct calls)

	if (verbose > 1)
	{	printf("\n=====Phase1 -- mark external sources\n");
	}

	cnt = step1_find_sources();	// check the most common types; mark ExternalSource

	if (!cnt)
	{	printf("taint: no sources of external input found\n");
		if (!p_debug)
		{	return;
		} else
		{	printf("taint: debug mode, continuing scan anyway\n");
	}	}

	// first iteration, to mark all derived external sources

	while (cnt)
	{	cnt = step2a_pre_scope();	// mark DerivedFromExternal	| parallel
		step2b_check_scope();		// mark DerivedFromExternal	| parallel
		cnt += step2c_step6_iterate();	// mark PropViaFct		| parallel
	}

	// once: check how the marked vars may be used
	// in dangerous calls, and mark dubious cases
	step3_step7_check_uses(VulnerableTarget);	// does any appear in dangerous calls
	save_set(3);	// uses of VulnerableTarget     // eg in memcpy, strcpy, sprintf

	// ==== done with potentially dangerous external sources  ====

	// phase 2 of 3
	// look for fixed size buffers declared on the stack
	// these can be targets for code injection
	// pattern:  @type ^;* @ident [   ^;* ;
	if (verbose > 1)
	{	printf("\n=====Phase2 -- mark taintable targets\n");
	}

	cnt  = step4_find_targets();	// fixed size buffers declared on stack, marked Target
					// stack variables assigned with alloca; marked Alloca

	if (cnt == 0)
	{	printf("taint: no taintable targets found\n");
		return;
	}

	// find anything that can point to Target or Alloca

	step5_find_taints();	// mark: UseOfTaint, DerivedFromTaint	| not parallel, but not expensive

	// check propagation of marked vars thru fct params or returns
	// and mark vars derived from these

	reset_tables();

	while (step2c_step6_iterate() != 0)	// mark PropViaFct	| parallel
	{	;
	}

	// if any marked variables are there
	// now check if they can get assigned/filled with data
	// coming from untrusted sources
	// e.g., in sprintf, strcpy, or memcpy calls

	step3_step7_check_uses(CorruptableSource);
	// all potentially dangerous targets that appear in copy routines
	// are now marked CorruptableSource

	save_set(1);	// uses of CorruptableSource | parallel

	// phase 3 of 3
	// check if the msets 1 and 3 overlap
	// meaning that an external source can be
	// used in a dangerous operation for a fct-local buffer
	if (verbose > 1)
	{	printf("\n=====Phase3 -- check overlap\n");
	}

	step8_check_contamination(1, 3);	// assignment of Sources to Targets | not parallel

//	in verbose mode, subtract ngets and nfopen from warnings
	if (verbose || !no_display || !no_match)	// not terse
	{	warnings -= (ngets + nfopen + ngets_bad);
	}
	if (ngets)
	{	printf("%3d calls to gets() noted, %d writing to taintable objects\n", ngets, ngets_bad);
		// the ngets_bad are reported separately as well, and add to the count in 'warnings'
	}
	if (nfopen)
	{	printf("%3d uses of potentially tainted arguments in calls to fopen()\n", nfopen);
	}
	if (warnings)
	{	printf("%3d %staint warnings\n", warnings, (ngets+nfopen>0)?"other ":"");
		printf("%3d total warnings\n", warnings+ngets+nfopen+ngets_bad);
	}
}
