# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"
# include "control.h"
# include "csupport.h"
# include "table.h"

# ifdef DEBUG
# undef EXTRA_STACK
# define EXTRA_STACK	0
# endif


static value stack[MIN_STACK];	/* initial stack */
static frame topframe;		/* top frame */
frame *cframe;			/* current frame */
static char *creator;		/* creator function name */
static unsigned int clen;	/* creator function name length */

int nil_type;			/* type of nil value */
value zero_int = { T_INT, TRUE };
value zero_float = { T_FLOAT, TRUE };
value nil_value = { T_NIL, TRUE };

/*
 * NAME:	interpret->init()
 * DESCRIPTION:	initialize the interpreter
 */
void i_init(create, nilisnot0)
char *create;
bool nilisnot0;
{
    topframe.fp = topframe.sp = stack + MIN_STACK;
    topframe.stack = topframe.prev_lip = topframe.lip = stack;
    topframe.nodepth = TRUE;
    topframe.noticks = TRUE;
    cframe = &topframe;

    creator = create;
    clen = strlen(create);

    nil_value.type = nil_type = (nilisnot0) ? T_NIL : T_INT;
}

/*
 * NAME:	interpret->ref_value()
 * DESCRIPTION:	reference a value
 */
void i_ref_value(v)
register value *v;
{
    switch (v->type) {
    case T_STRING:
	str_ref(v->u.string);
	break;

    case T_OBJECT:
	if (DESTRUCTED(v)) {
	    *v = nil_value;
	}
	break;

    case T_ARRAY:
    case T_MAPPING:
	arr_ref(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->del_value()
 * DESCRIPTION:	dereference a value (not an lvalue)
 */
void i_del_value(v)
register value *v;
{
    switch (v->type) {
    case T_STRING:
	str_del(v->u.string);
	break;

    case T_ARRAY:
    case T_MAPPING:
	arr_del(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->copy()
 * DESCRIPTION:	copy values from one place to another
 */
void i_copy(v, w, len)
register value *v, *w;
register unsigned int len;
{
    while (len != 0) {
	i_ref_value(w);
	*v++ = *w++;
	--len;
    }
}

/*
 * NAME:	interpret->grow_stack()
 * DESCRIPTION:	check if there is room on the stack for new values; if not,
 *		make space
 */
void i_grow_stack(f, size)
register frame *f;
int size;
{
    if (f->sp < f->lip + size + MIN_STACK) {
	register int spsize, lisize;
	register value *v, *stk;
	register long offset;

	/*
	 * extend the local stack
	 */
	spsize = f->fp - f->sp;
	lisize = f->lip - f->stack;
	size = ALGN(spsize + lisize + size + MIN_STACK, 8);
	stk = ALLOC(value, size);
	offset = (long) (stk + size) - (long) f->fp;

	/* move lvalue index stack values */
	if (lisize != 0) {
	    memcpy(stk, f->stack, lisize * sizeof(value));
	}
	f->lip = stk + lisize;

	/* move stack values */
	v = stk + size;
	if (spsize != 0) {
	    memcpy(v - spsize, f->sp, spsize * sizeof(value));
	    do {
		--v;
		if ((v->type == T_LVALUE || v->type == T_SLVALUE) &&
		    v->u.lval >= f->sp && v->u.lval < f->fp) {
		    v->u.lval = (value *) ((long) v->u.lval + offset);
		}
	    } while (--spsize > 0);
	}
	f->sp = v;

	/* replace old stack */
	if (f->sos) {
	    /* stack on stack: alloca'd */
	    AFREE(f->stack);
	    f->sos = FALSE;
	} else if (f->stack != stack) {
	    FREE(f->stack);
	}
	f->stack = stk;
	f->fp = stk + size;
    }
}

/*
 * NAME:	interpret->push_value()
 * DESCRIPTION:	push a value on the stack
 */
void i_push_value(f, v)
frame *f;
register value *v;
{
    *--f->sp = *v;
    switch (v->type) {
    case T_STRING:
	str_ref(v->u.string);
	break;

    case T_OBJECT:
	if (DESTRUCTED(v)) {
	    /*
	     * can't wipe out the original, since it may be a value from a
	     * mapping
	     */
	    *f->sp = nil_value;
	}
	break;

    case T_ARRAY:
    case T_MAPPING:
	arr_ref(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->pop()
 * DESCRIPTION:	pop a number of values (can be lvalues) from the stack
 */
void i_pop(f, n)
register frame *f;
register int n;
{
    register value *v;

    for (v = f->sp; --n >= 0; v++) {
	switch (v->type) {
	case T_STRING:
	    str_del(v->u.string);
	    break;

	case T_ALVALUE:
	    --f->lip;
	case T_ARRAY:
	case T_MAPPING:
	    arr_del(v->u.array);
	    break;

	case T_SLVALUE:
	    --f->lip;
	    break;

	case T_MLVALUE:
	    i_del_value(--f->lip);
	    arr_del(v->u.array);
	    break;

	case T_SALVALUE:
	    f->lip -= 2;
	    arr_del(v->u.array);
	    break;

	case T_SMLVALUE:
	    f->lip -= 2;
	    i_del_value(f->lip);
	    arr_del(v->u.array);
	    break;
	}
    }
    f->sp = v;
}

/*
 * NAME:	interpret->reverse()
 * DESCRIPTION:	reverse the order of arguments on the stack
 */
void i_reverse(f, n)
frame *f;
register int n;
{
    value sp[MAX_LOCALS];
    value lip[MAX_LOCALS];
    register value *v1, *v2, *w1, *w2;

    if (n > 1) {
	/*
	 * more than one argument
	 */
	v1 = f->sp;
	v2 = sp;
	w1 = lip;
	w2 = f->lip;
	memcpy(v2, v1, n * sizeof(value));
	v1 += n;

	do {
	    switch (v2->type) {
	    case T_SLVALUE:
	    case T_ALVALUE:
	    case T_MLVALUE:
		*w1++ = *--w2;
		break;

	    case T_SALVALUE:
	    case T_SMLVALUE:
		w2 -= 2;
		*w1++ = w2[0];
		*w1++ = w2[1];
		break;
	    }

	    *--v1 = *v2++;
	} while (--n != 0);

	/*
	 * copy back lvalue indices, if needed
	 */
	n = f->lip - w2;
	if (n > 1) {
	    memcpy(w2, lip, n * sizeof(value));
	}
    }
}

/*
 * NAME:	interpret->odest()
 * DESCRIPTION:	replace all occurrances of an object on the stack by 0
 */
void i_odest(ftop, obj)
frame *ftop;
object *obj;
{
    register Uint count;
    register value *v;
    register frame *f;

    count = obj->count;

    /* wipe out objects in stack frames */
    v = ftop->sp;
    for (f = ftop; f != (frame *) NULL; f = f->prev) {
	while (v < f->fp) {
	    if (v->type == T_OBJECT && v->u.objcnt == count) {
		*v = nil_value;
	    }
	    v++;
	}
	v = f->argp;
    }
    /* wipe out objects in lvalue index stack */
    v = ftop->lip;
    for (f = ftop; f != (frame *) NULL; f = f->prev) {
	while (v >= f->stack) {
	    if (v->type == T_OBJECT && v->u.objcnt == count) {
		*v = nil_value;
	    }
	    --v;
	}
	v = f->prev_lip;
    }
}

/*
 * NAME:	interpret->string()
 * DESCRIPTION:	push a string constant on the stack
 */
void i_string(f, inherit, index)
frame *f;
int inherit;
unsigned int index;
{
    (--f->sp)->type = T_STRING;
    str_ref(f->sp->u.string = d_get_strconst(f->p_ctrl, inherit, index));
}

/*
 * NAME:	interpret->aggregate()
 * DESCRIPTION:	create an array on the stack
 */
void i_aggregate(f, size)
register frame *f;
register unsigned int size;
{
    register array *a;

    if (size == 0) {
	a = arr_new(f->data, 0L);
    } else {
	register value *v, *elts;

	i_add_ticks(f, size);
	a = arr_new(f->data, (long) size);
	elts = a->elts + size;
	v = f->sp;
	do {
	    *--elts = *v++;
	} while (--size != 0);
	d_ref_imports(a);
	f->sp = v;
    }
    (--f->sp)->type = T_ARRAY;
    arr_ref(f->sp->u.array = a);
}

/*
 * NAME:	interpret->map_aggregate()
 * DESCRIPTION:	create a mapping on the stack
 */
void i_map_aggregate(f, size)
register frame *f;
register unsigned int size;
{
    register array *a;

    if (size == 0) {
	a = map_new(f->data, 0L);
    } else {
	register value *v, *elts;

	i_add_ticks(f, size);
	a = map_new(f->data, (long) size);
	elts = a->elts + size;
	v = f->sp;
	do {
	    *--elts = *v++;
	} while (--size != 0);
	f->sp = v;
	if (ec_push((ec_ftn) NULL)) {
	    /* error in sorting, delete mapping and pass on error */
	    arr_ref(a);
	    arr_del(a);
	    error((char *) NULL);
	}
	map_sort(a);
	ec_pop();
	d_ref_imports(a);
    }
    (--f->sp)->type = T_MAPPING;
    arr_ref(f->sp->u.array = a);
}

/*
 * NAME:	interpret->spread()
 * DESCRIPTION:	push the values in an array on the stack, return the size
 *		of the array - 1
 */
int i_spread(f, n, vtype)
register frame *f;
register int n, vtype;
{
    register array *a;
    register int i;
    register value *v;

    if (f->sp->type != T_ARRAY) {
	error("Spread of non-array");
    }
    a = f->sp->u.array;
    if (n < 0 || n > a->size) {
	/* no lvalues */
	n = a->size;
    }
    if (a->size > 0) {
	i_add_ticks(f, a->size);
	i_grow_stack(f, (a->size << 1) - n - 1);
	a->ref += a->size - n;
    }
    f->sp++;

    /* values */
    for (i = 0, v = d_get_elts(a); i < n; i++, v++) {
	i_push_value(f, v);
    }
    /* lvalues */
    for (n = a->size; i < n; i++) {
	(--f->sp)->type = T_ALVALUE;
	f->sp->oindex = vtype;
	f->sp->u.array = a;
	f->lip->type = T_INT;
	(f->lip++)->u.number = i;
    }

    arr_del(a);
    return n - 1;
}

/*
 * NAME:	interpret->global()
 * DESCRIPTION:	push a global value on the stack
 */
void i_global(f, inherit, index)
register frame *f;
register int inherit, index;
{
    i_add_ticks(f, 4);
    if (inherit != 0) {
	inherit = f->ctrl->inherits[f->p_index - inherit].varoffset;
    }
    i_push_value(f, d_get_variable(f->data, inherit + index));
}

/*
 * NAME:	interpret->global_lvalue()
 * DESCRIPTION:	push a global lvalue on the stack
 */
void i_global_lvalue(f, inherit, index, vtype)
register frame *f;
register int inherit;
int index, vtype;
{
    i_add_ticks(f, 4);
    if (inherit != 0) {
	inherit = f->ctrl->inherits[f->p_index - inherit].varoffset;
    }
    (--f->sp)->type = T_LVALUE;
    f->sp->oindex = vtype;
    f->sp->u.lval = d_get_variable(f->data, inherit + index);
}

/*
 * NAME:	interpret->index()
 * DESCRIPTION:	index a value, REPLACING it by the indexed value
 */
void i_index(f)
register frame *f;
{
    register int i;
    register value *aval, *ival, *val;
    array *a;

    i_add_ticks(f, 2);
    ival = f->sp++;
    aval = f->sp;
    switch (aval->type) {
    case T_STRING:
	if (ival->type != T_INT) {
	    i_del_value(ival);
	    error("Non-numeric string index");
	}
	i = UCHAR(aval->u.string->text[str_index(aval->u.string,
						 (long) ival->u.number)]);
	str_del(aval->u.string);
	aval->type = T_INT;
	aval->u.number = i;
	return;

    case T_ARRAY:
	if (ival->type != T_INT) {
	    i_del_value(ival);
	    error("Non-numeric array index");
	}
	val = &d_get_elts(aval->u.array)[arr_index(aval->u.array,
						   (long) ival->u.number)];
	break;

    case T_MAPPING:
	val = map_index(aval->u.array, ival, (value *) NULL);
	i_del_value(ival);
	break;

    default:
	i_del_value(ival);
	error("Index on bad type");
    }

    a = aval->u.array;
    switch (val->type) {
    case T_STRING:
	str_ref(val->u.string);
	break;

    case T_OBJECT:
	if (DESTRUCTED(val)) {
	    val = &nil_value;
	}
	break;

    case T_ARRAY:
    case T_MAPPING:
	arr_ref(val->u.array);
	break;
    }
    *aval = *val;
    arr_del(a);
}

/*
 * NAME:	interpret->index_lvalue()
 * DESCRIPTION:	Index a value, REPLACING it by an indexed lvalue.
 */
void i_index_lvalue(f, vtype)
register frame *f;
int vtype;
{
    register int i;
    register value *lval, *ival, *val;

    i_add_ticks(f, 2);
    ival = f->sp++;
    lval = f->sp;
    switch (lval->type) {
    case T_STRING:
	/* for instance, "foo"[1] = 'a'; */
	i_del_value(ival);
	error("Bad lvalue");

    case T_ARRAY:
	if (ival->type != T_INT) {
	    i_del_value(ival);
	    error("Non-numeric array index");
	}
	i = arr_index(lval->u.array, (long) ival->u.number);
	lval->type = T_ALVALUE;
	lval->oindex = vtype;
	f->lip->type = T_INT;
	(f->lip++)->u.number = i;
	return;

    case T_MAPPING:
	lval->type = T_MLVALUE;
	lval->oindex = vtype;
	*f->lip++ = *ival;
	return;

    case T_LVALUE:
	/*
	 * note: the lvalue is not yet referenced
	 */
	switch (lval->u.lval->type) {
	case T_STRING:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric string index");
	    }
	    i = str_index(f->lvstr = lval->u.lval->u.string,
			  (long) ival->u.number);
	    /* indexed string lvalues are not referenced */
	    lval->type = T_SLVALUE;
	    lval->oindex = vtype;
	    f->lip->type = T_INT;
	    (f->lip++)->u.number = i;
	    return;

	case T_ARRAY:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric array index");
	    }
	    i = arr_index(lval->u.lval->u.array, (long) ival->u.number);
	    lval->type = T_ALVALUE;
	    lval->oindex = vtype;
	    arr_ref(lval->u.array = lval->u.lval->u.array);
	    f->lip->type = T_INT;
	    (f->lip++)->u.number = i;
	    return;

	case T_MAPPING:
	    lval->type = T_MLVALUE;
	    lval->oindex = vtype;
	    arr_ref(lval->u.array = lval->u.lval->u.array);
	    *f->lip++ = *ival;
	    return;
	}
	break;

    case T_ALVALUE:
	val = &d_get_elts(lval->u.array)[f->lip[-1].u.number];
	switch (val->type) {
	case T_STRING:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric string index");
	    }
	    i = str_index(f->lvstr = val->u.string, (long) ival->u.number);
	    lval->type = T_SALVALUE;
	    lval->oindex = vtype;
	    f->lip->type = T_INT;
	    (f->lip++)->u.number = i;
	    return;

	case T_ARRAY:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric array index");
	    }
	    i = arr_index(val->u.array, (long) ival->u.number);
	    arr_ref(val->u.array);	/* has to be first */
	    arr_del(lval->u.array);	/* has to be second */
	    lval->oindex = vtype;
	    lval->u.array = val->u.array;
	    f->lip[-1].u.number = i;
	    return;

	case T_MAPPING:
	    arr_ref(val->u.array);	/* has to be first */
	    arr_del(lval->u.array);	/* has to be second */
	    lval->type = T_MLVALUE;
	    lval->oindex = vtype;
	    lval->u.array = val->u.array;
	    f->lip[-1] = *ival;
	    return;
	}
	break;

    case T_MLVALUE:
	val = map_index(lval->u.array, &f->lip[-1], (value *) NULL);
	switch (val->type) {
	case T_STRING:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric string index");
	    }
	    i = str_index(f->lvstr = val->u.string, (long) ival->u.number);
	    lval->type = T_SMLVALUE;
	    lval->oindex = vtype;
	    f->lip->type = T_INT;
	    (f->lip++)->u.number = i;
	    return;

	case T_ARRAY:
	    if (ival->type != T_INT) {
		i_del_value(ival);
		error("Non-numeric array index");
	    }
	    i = arr_index(val->u.array, (long) ival->u.number);
	    arr_ref(val->u.array);	/* has to be first */
	    arr_del(lval->u.array);	/* has to be second */
	    lval->type = T_ALVALUE;
	    lval->oindex = vtype;
	    lval->u.array = val->u.array;
	    i_del_value(&f->lip[-1]);
	    f->lip[-1].type = T_INT;
	    f->lip[-1].u.number = i;
	    return;

	case T_MAPPING:
	    arr_ref(val->u.array);	/* has to be first */
	    arr_del(lval->u.array);	/* has to be second */
	    lval->oindex = vtype;
	    lval->u.array = val->u.array;
	    i_del_value(&f->lip[-1]);
	    f->lip[-1] = *ival;
	    return;
	}
	break;
    }
    i_del_value(ival);
    error("Index on bad type");
}

/*
 * NAME:	interpret->typename()
 * DESCRIPTION:	return the name of the argument type
 */
char *i_typename(buf, type)
register char *buf;
register unsigned int type;
{
    static char *name[] = TYPENAMES;

    strcpy(buf, name[type & T_TYPE]);
    type &= T_REF;
    type >>= REFSHIFT;
    if (type > 0) {
	register char *p;

	p = buf + strlen(buf);
	*p++ = ' ';
	do {
	    *p++ = '*';
	} while (--type > 0);
	*p = '\0';
    }
    return buf;
}

/*
 * NAME:	interpret->cast()
 * DESCRIPTION:	cast a value to a type
 */
void i_cast(val, type)
register value *val;
register unsigned int type;
{
    char tnbuf[8];

    if (val->type != type && (!VAL_NIL(val) || !T_POINTER(type))) {
	i_typename(tnbuf, type);
	if (strchr("aeiuoy", tnbuf[0]) != (char *) NULL) {
	    error("Value is not an %s", tnbuf);
	} else {
	    error("Value is not a %s", tnbuf);
	}
    }
}

/*
 * NAME:	interpret->fetch()
 * DESCRIPTION:	fetch the value of an lvalue
 */
void i_fetch(f)
register frame *f;
{
    switch (f->sp->type) {
    case T_LVALUE:
	i_push_value(f, f->sp->u.lval);
	break;

    case T_ALVALUE:
	i_push_value(f, d_get_elts(f->sp->u.array) + f->lip[-1].u.number);
	break;

    case T_MLVALUE:
	i_push_value(f, map_index(f->sp->u.array, &f->lip[-1],
				  (value *) NULL));
	break;

    default:
	/*
         * Indexed string.
         * The fetch is always done directly after an lvalue
         * constructor, so lvstr is valid.
         */
	(--f->sp)->type = T_INT;
	f->sp->u.number = UCHAR(f->lvstr->text[f->lip[-1].u.number]);
	break;
    }
}

/*
 * NAME:	istr()
 * DESCRIPTION:	create a copy of the argument string, with one char replaced
 */
static value *istr(val, str, i, v)
register value *val, *v;
register string *str;
unsigned short i;
{
    if (v->type != T_INT) {
	error("Non-numeric value in indexed string assignment");
    }

    val->type = T_STRING;
    if (str->primary == (strref *) NULL && str->ref == 1) {
	/* only reference to this string: don't copy */
	val->u.string = str;
    } else {
	val->u.string = str_new(str->text, (long) str->len);
    }
    val->u.string->text[i] = v->u.number;
    return val;
}

/*
 * NAME:	interpret->store()
 * DESCRIPTION:	Perform an assignment. This invalidates the lvalue.
 */
void i_store(f)
register frame *f;
{
    value ival;
    register value *lval, *val, *v;
    register unsigned short i;
    register array *a;

    lval = f->sp + 1;
    val = f->sp;
    if (lval->oindex != 0) {
	i_cast(val, lval->oindex);
    }

    i_add_ticks(f, 1);
    switch (lval->type) {
    case T_LVALUE:
	d_assign_var(f->data, lval->u.lval, val);
	break;

    case T_SLVALUE:
	v = lval->u.lval;
	i = f->lip[-1].u.number;
	if (v->type != T_STRING || i >= v->u.string->len) {
	    /*
	     * The lvalue was changed.
	     */
	    error("Lvalue disappeared!");
	}
	--f->lip;
	d_assign_var(f->data, v, istr(&ival, v->u.string, i, val));
	break;

    case T_ALVALUE:
	a = lval->u.array;
	d_assign_elt(a, &d_get_elts(a)[(--f->lip)->u.number], val);
	arr_del(a);
	break;

    case T_MLVALUE:
	map_index(a = lval->u.array, &f->lip[-1], val);
	i_del_value(--f->lip);
	arr_del(a);
	break;

    case T_SALVALUE:
	a = lval->u.array;
	v = &a->elts[f->lip[-2].u.number];
	i = f->lip[-1].u.number;
	if (v->type != T_STRING || i >= v->u.string->len) {
	    /*
	     * The lvalue was changed.
	     */
	    error("Lvalue disappeared!");
	}
	d_assign_elt(a, v, istr(&ival, v->u.string, i, val));
	f->lip -= 2;
	arr_del(a);
	break;

    case T_SMLVALUE:
	a = lval->u.array;
	v = map_index(a, &f->lip[-2], (value *) NULL);
	i = f->lip[-1].u.number;
	if (v->type != T_STRING || i >= v->u.string->len) {
	    /*
	     * The lvalue was changed.
	     */
	    error("Lvalue disappeared!");
	}
	d_assign_elt(a, v, istr(&ival, v->u.string, i, val));
	f->lip -= 2;
	i_del_value(f->lip);
	arr_del(a);
	break;
    }
}

typedef struct {
    Int depth;		/* stack depth */
    Int ticks;		/* ticks left */
    bool nodepth;	/* no depth checking */
    bool noticks;	/* no ticks checking */
} rlinfo;

static rlinfo rlstack[ERRSTACKSZ];	/* rlimits stack */

/*
 * NAME:	interpret->get_depth()
 * DESCRIPTION:	get the remaining stack depth (-1: infinite)
 */
Int i_get_depth(f)
register frame *f;
{
    if (f->nodepth) {
	return -1;
    }
    return f->maxdepth - f->depth;
}

/*
 * NAME:	interpret->get_ticks()
 * DESCRIPTION:	get the remaining ticks (-1: infinite)
 */
Int i_get_ticks(f)
register frame *f;
{
    if (f->noticks) {
	return -1;
    } else {
	return (f->ticks < 0) ? 0 : f->ticks;
    }
}

/*
 * NAME:	interpret->check_rlimits()
 * DESCRIPTION:	check if this rlimits call is valid
 */
static void i_check_rlimits(f)
register frame *f;
{
    if (f->obj->count == 0) {
	error("Illegal use of rlimits");
    }
    --f->sp;
    f->sp[0] = f->sp[1];
    f->sp[1] = f->sp[2];
    f->sp[2].type = T_OBJECT;
    f->sp[2].oindex = f->obj->index;
    f->sp[2].u.objcnt = f->obj->count;
    /* obj, stack, ticks */
    call_driver_object(f, "runtime_rlimits", 3);

    if (!VAL_TRUE(f->sp)) {
	error("Illegal use of rlimits");
    }
    i_del_value(f->sp++);
}

/*
 * NAME:	interpret->set_rlimits()
 * DESCRIPTION:	set new rlimits.  Return an integer that can be used in
 *		restoring the old values
 */
int i_set_rlimits(f, depth, t)
register frame *f;
Int depth, t;
{
    if (f->rli == ERRSTACKSZ) {
	error("Too deep rlimits nesting");
    }
    rlstack[f->rli].depth = f->maxdepth;
    rlstack[f->rli].nodepth = f->nodepth;
    rlstack[f->rli].noticks = f->noticks;

    if (depth != 0) {
	if (depth < 0) {
	    f->nodepth = TRUE;
	} else {
	    f->maxdepth = f->depth + depth;
	    f->nodepth = FALSE;
	}
    }
    if (t != 0) {
	if (t < 0) {
	    rlstack[f->rli].ticks = f->ticks;
	    f->noticks = TRUE;
	} else {
	    rlstack[f->rli].ticks = f->ticks - t;
	    f->ticks = t;
	    f->noticks = FALSE;
	}
    } else {
	rlstack[f->rli].ticks = 0;
    }

    return f->rli++;
}

/*
 * NAME:	interpret->get_rllevel()
 * DESCRIPTION:	return the current rlimits stack level
 */
int i_get_rllevel(f)
frame *f;
{
    return f->rli;
}

/*
 * NAME:	interpret->set_rllevel()
 * DESCRIPTION:	restore rlimits to an earlier level
 */
void i_set_rllevel(f, n)
register frame *f;
int n;
{
    if (n < 0) {
	n += f->rli;
    }
    if (f->ticks < 0) {
	f->ticks = 0;
    }
    while (f->rli > n) {
	--f->rli;
	f->maxdepth = rlstack[f->rli].depth;
	if (f->noticks) {
	    f->ticks = rlstack[f->rli].ticks;
	} else {
	    f->ticks += rlstack[f->rli].ticks;
	}
	f->nodepth = rlstack[f->rli].nodepth;
	f->noticks = rlstack[f->rli].noticks;
    }
}

/*
 * NAME:	interpret->set_sp()
 * DESCRIPTION:	set the current stack pointer
 */
frame *i_set_sp(ftop, sp)
frame *ftop;
register value *sp;
{
    register value *v, *w;
    register frame *f;

    v = ftop->sp;
    w = ftop->lip;
    for (f = ftop; f != NULL; f = f->prev) {
	for (;;) {
	    if (v == sp) {
		f->sp = v;
		f->lip = w;
		f->ticks = ftop->ticks;
		return f;
	    }
	    if (v == f->fp) {
		break;
	    }
	    switch (v->type) {
	    case T_STRING:
		str_del(v->u.string);
		break;

	    case T_SLVALUE:
		--w;
		break;

	    case T_ALVALUE:
		--w;
	    case T_ARRAY:
	    case T_MAPPING:
		arr_del(v->u.array);
		break;

	    case T_MLVALUE:
		i_del_value(--w);
		arr_del(v->u.array);
		break;

	    case T_SALVALUE:
		w -= 2;
		arr_del(v->u.array);
		break;

	    case T_SMLVALUE:
		w -= 2;
		i_del_value(w);
		arr_del(v->u.array);
		break;
	    }
	    v++;
	}
	v = f->argp;
	w = f->prev_lip;

	if (f->sos) {
	    /* stack on stack */
	    AFREE(f->stack);
	} else if (f->obj != (object *) NULL) {
	    FREE(f->stack);
	}
    }

    f->sp = v;
    f->lip = w;
    f->ticks = ftop->ticks;
    return f;
}

/*
 * NAME:	interpret->prev_object()
 * DESCRIPTION:	return the nth previous object in the call_other chain
 */
object *i_prev_object(f, n)
register frame *f;
register int n;
{
    while (n >= 0) {
	/* back to last external call */
	while (!f->external) {
	    f = f->prev;
	}
	f = f->prev;
	if (f->obj == (object *) NULL) {
	    return (object *) NULL;
	}
	--n;
    }
    return (f->obj->count == 0) ? (object *) NULL : f->obj;
}

/*
 * NAME:	interpret->prev_program()
 * DESCRIPTION:	return the nth previous program in the function call chain
 */
char *i_prev_program(f, n)
register frame *f;
register int n;
{
    while (n >= 0) {
	f = f->prev;
	if (f->obj == (object *) NULL) {
	    return (char *) NULL;
	}
	--n;
    }

    return f->p_ctrl->obj->chain.name;
}

/*
 * NAME:	interpret->typecheck()
 * DESCRIPTION:	check the argument types given to a function
 */
void i_typecheck(f, name, ftype, proto, nargs, strict)
register frame *f;
char *name, *ftype;
register char *proto;
int nargs;
int strict;
{
    char tnbuf[8];
    register int i, n, atype, ptype;
    register char *args;

    i = nargs;
    n = PROTO_NARGS(proto);
    args = PROTO_ARGS(proto);
    while (n > 0 && i > 0) {
	--i;
	ptype = UCHAR(*args);
	if (ptype & T_ELLIPSIS) {
	    ptype &= ~T_ELLIPSIS;
	    if (ptype == T_MIXED || ptype == T_LVALUE) {
		return;
	    }
	} else {
	    args++;
	    --n;
	}

	if (ptype != T_MIXED) {
	    atype = f->sp[i].type;
	    if (ptype != atype && (atype != T_ARRAY || !(ptype & T_REF))) {
		if (!VAL_NIL(f->sp + i) || !T_POINTER(ptype)) {
		    /* wrong type */
		    error("Bad argument %d (%s) for %s %s", nargs - i,
			  i_typename(tnbuf, atype), ftype, name);
		} else if (strict) {
		    /* zero argument */
		    error("Bad argument %d for %s %s", nargs - i, ftype, name);
		}
	    }
	}
    }
}

/*
 * NAME:	interpret->switch_int()
 * DESCRIPTION:	handle an int switch
 */
static unsigned short i_switch_int(f, pc)
register frame *f;
register char *pc;
{
    register unsigned short h, l, m, sz, dflt;
    register Int num;
    register char *p;

    FETCH2U(pc, h);
    sz = FETCH1U(pc);
    FETCH2U(pc, dflt);
    if (f->sp->type != T_INT) {
	return dflt;
    }

    l = 0;
    --h;
    switch (sz) {
    case 1:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 3 * m;
	    num = FETCH1S(p);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 2:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 4 * m;
	    FETCH2S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 3:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 5 * m;
	    FETCH3S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 4:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 6 * m;
	    FETCH4S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;
    }

    return dflt;
}

/*
 * NAME:	interpret->switch_range()
 * DESCRIPTION:	handle a range switch
 */
static unsigned short i_switch_range(f, pc)
register frame *f;
register char *pc;
{
    register unsigned short h, l, m, sz, dflt;
    register Int num;
    register char *p;

    FETCH2U(pc, h);
    sz = FETCH1U(pc);
    FETCH2U(pc, dflt);
    if (f->sp->type != T_INT) {
	return dflt;
    }

    l = 0;
    --h;
    switch (sz) {
    case 1:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 4 * m;
	    num = FETCH1S(p);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		num = FETCH1S(p);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 2:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 6 * m;
	    FETCH2S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH2S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 3:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 8 * m;
	    FETCH3S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH3S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 4:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 10 * m;
	    FETCH4S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH4S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;
    }
    return dflt;
}

/*
 * NAME:	interpret->switch_str()
 * DESCRIPTION:	handle a string switch
 */
static unsigned short i_switch_str(f, pc)
register frame *f;
register char *pc;
{
    register unsigned short h, l, m, u, u2, dflt;
    register int cmp;
    register char *p;
    register control *ctrl;

    FETCH2U(pc, h);
    FETCH2U(pc, dflt);
    if (FETCH1U(pc) == 0) {
	FETCH2U(pc, l);
	if (VAL_NIL(f->sp)) {
	    return l;
	}
	--h;
    }
    if (f->sp->type != T_STRING) {
	return dflt;
    }

    ctrl = f->p_ctrl;
    l = 0;
    --h;
    while (l < h) {
	m = (l + h) >> 1;
	p = pc + 5 * m;
	u = FETCH1U(p);
	cmp = str_cmp(f->sp->u.string, d_get_strconst(ctrl, u, FETCH2U(p, u2)));
	if (cmp == 0) {
	    return FETCH2U(p, l);
	} else if (cmp < 0) {
	    h = m;	/* search in lower half */
	} else {
	    l = m + 1;	/* search in upper half */
	}
    }
    return dflt;
}

/*
 * NAME:	interpret->catcherr()
 * DESCRIPTION:	handle caught error
 */
void i_catcherr(f, depth)
frame *f;
Int depth;
{
    i_runtime_error(f, depth + 1);
}

/*
 * NAME:	interpret->interpret()
 * DESCRIPTION:	Main interpreter function. Interpret stack machine code.
 */
static void i_interpret(f, pc)
register frame *f;
register char *pc;
{
    register unsigned short instr, u, u2;
    register Uint l;
    register char *p;
    register kfunc *kf;
    xfloat flt;
    int size;
    Int newdepth, newticks;

    size = 0;

    for (;;) {
# ifdef DEBUG
	if (f->sp < f->lip + MIN_STACK) {
	    fatal("out of value stack");
	}
# endif
	if (--f->ticks <= 0) {
	    if (f->noticks) {
		f->ticks = 0x7fffffff;
	    } else {
		error("Out of ticks");
	    }
	}
	instr = FETCH1U(pc);
	f->pc = pc;

	switch (instr & I_INSTR_MASK) {
	case I_PUSH_ZERO:
	    *--f->sp = zero_int;
	    break;

	case I_PUSH_ONE:
	    (--f->sp)->type = T_INT;
	    f->sp->u.number = 1;
	    break;

	case I_PUSH_INT1:
	    (--f->sp)->type = T_INT;
	    f->sp->u.number = FETCH1S(pc);
	    break;

	case I_PUSH_INT4:
	    (--f->sp)->type = T_INT;
	    f->sp->u.number = FETCH4S(pc, l);
	    break;

	case I_PUSH_FLOAT:
	    (--f->sp)->type = T_FLOAT;
	    flt.high = FETCH2U(pc, u);
	    flt.low = FETCH4U(pc, l);
	    VFLT_PUT(f->sp, flt);
	    break;

	case I_PUSH_STRING:
	    (--f->sp)->type = T_STRING;
	    str_ref(f->sp->u.string = d_get_strconst(f->p_ctrl,
						     f->p_ctrl->ninherits - 1,
						     FETCH1U(pc)));
	    break;

	case I_PUSH_NEAR_STRING:
	    (--f->sp)->type = T_STRING;
	    u = FETCH1U(pc);
	    str_ref(f->sp->u.string = d_get_strconst(f->p_ctrl, u,
						     FETCH1U(pc)));
	    break;

	case I_PUSH_FAR_STRING:
	    (--f->sp)->type = T_STRING;
	    u = FETCH1U(pc);
	    str_ref(f->sp->u.string = d_get_strconst(f->p_ctrl, u,
						     FETCH2U(pc, u2)));
	    break;

	case I_PUSH_LOCAL:
	    u = FETCH1S(pc);
	    i_push_value(f, ((short) u < 0) ? f->fp + (short) u : f->argp + u);
	    break;

	case I_PUSH_GLOBAL:
	    u = f->ctrl->inherits[f->p_index - 1].varoffset + FETCH1U(pc);
	    i_push_value(f, d_get_variable(f->data, u));
	    break;

	case I_PUSH_FAR_GLOBAL:
	    u = FETCH1U(pc);
	    if (u != 0) {
		u = f->ctrl->inherits[f->p_index - u].varoffset;
	    }
	    i_push_value(f, d_get_variable(f->data, u + FETCH1U(pc)));
	    break;

	case I_PUSH_LOCAL_LVAL:
	    (--f->sp)->type = T_LVALUE;
	    u = FETCH1S(pc);
	    f->sp->oindex = (instr & I_TYPE_BIT) ? FETCH1U(pc) : 0;
	    f->sp->u.lval = ((short) u < 0) ? f->fp + (short) u : f->argp + u;
	    continue;

	case I_PUSH_GLOBAL_LVAL:
	    u = f->ctrl->inherits[f->p_index - 1].varoffset + FETCH1U(pc);
	    (--f->sp)->type = T_LVALUE;
	    f->sp->oindex = (instr & I_TYPE_BIT) ? FETCH1U(pc) : 0;
	    f->sp->u.lval = d_get_variable(f->data, u);
	    continue;

	case I_PUSH_FAR_GLOBAL_LVAL:
	    u = FETCH1U(pc);
	    if (u != 0) {
		u = f->ctrl->inherits[f->p_index - u].varoffset;
	    }
	    u += FETCH1U(pc);
	    (--f->sp)->type = T_LVALUE;
	    f->sp->oindex = (instr & I_TYPE_BIT) ? FETCH1U(pc) : 0;
	    f->sp->u.lval = d_get_variable(f->data, u);
	    continue;

	case I_INDEX:
	    i_index(f);
	    break;

	case I_INDEX_LVAL:
	    i_index_lvalue(f, (instr & I_TYPE_BIT) ? FETCH1U(pc) : 0);
	    continue;

	case I_AGGREGATE:
	    if (FETCH1U(pc) == 0) {
		i_aggregate(f, FETCH2U(pc, u));
	    } else {
		i_map_aggregate(f, FETCH2U(pc, u));
	    }
	    break;

	case I_SPREAD:
	    u = FETCH1S(pc);
	    size = i_spread(f, (short) u,
			    (instr & I_TYPE_BIT) ? FETCH1U(pc) : 0);
	    continue;

	case I_CAST:
	    i_cast(f->sp, FETCH1U(pc));
	    break;

	case I_FETCH:
	    i_fetch(f);
	    break;

	case I_STORE:
	    i_store(f);
	    f->sp[1] = f->sp[0];
	    f->sp++;
	    break;

	case I_JUMP:
	    p = f->prog + FETCH2U(pc, u);
	    pc = p;
	    break;

	case I_JUMP_ZERO:
	    p = f->prog + FETCH2U(pc, u);
	    if (!VAL_TRUE(f->sp)) {
		pc = p;
	    }
	    break;

	case I_JUMP_NONZERO:
	    p = f->prog + FETCH2U(pc, u);
	    if (VAL_TRUE(f->sp)) {
		pc = p;
	    }
	    break;

	case I_SWITCH:
	    switch (FETCH1U(pc)) {
	    case SWITCH_INT:
		pc = f->prog + i_switch_int(f, pc);
		break;

	    case SWITCH_RANGE:
		pc = f->prog + i_switch_range(f, pc);
		break;

	    case SWITCH_STRING:
		pc = f->prog + i_switch_str(f, pc);
		break;
	    }
	    break;

	case I_CALL_KFUNC:
	    kf = &KFUN(FETCH1U(pc));
	    if (PROTO_CLASS(kf->proto) & C_VARARGS) {
		/* variable # of arguments */
		u = FETCH1U(pc) + size;
		size = 0;
	    } else {
		/* fixed # of arguments */
		u = PROTO_NARGS(kf->proto);
	    }
	    if (PROTO_CLASS(kf->proto) & C_TYPECHECKED) {
		i_typecheck(f, kf->name, "kfun", kf->proto, u, TRUE);
	    }
	    u = (*kf->func)(f, u);
	    if (u != 0) {
		if ((short) u < 0) {
		    error("Too few arguments for kfun %s", kf->name);
		} else if (u <= PROTO_NARGS(kf->proto)) {
		    error("Bad argument %d for kfun %s", u, kf->name);
		} else {
		    error("Too many arguments for kfun %s", kf->name);
		}
	    }
	    break;

	case I_CALL_AFUNC:
	    u = FETCH1U(pc);
	    i_funcall(f, (object *) NULL, 0, u, FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CALL_DFUNC:
	    u = FETCH1U(pc);
	    if (u != 0) {
		u = f->p_index - u;
	    }
	    u2 = FETCH1U(pc);
	    i_funcall(f, (object *) NULL, u, u2, FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CALL_FUNC:
	    p = &f->ctrl->funcalls[2L * (f->foffset + FETCH2U(pc, u))];
	    i_funcall(f, (object *) NULL, UCHAR(p[0]), UCHAR(p[1]),
		      FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CATCH:
	    p = f->prog + FETCH2U(pc, u);
	    u = f->rli;
	    if (!ec_push((ec_ftn) i_catcherr)) {
		i_interpret(f, pc);
		ec_pop();
		pc = f->pc;
		*--f->sp = nil_value;
	    } else {
		/* error */
		f->pc = pc = p;
		p = errormesg();
		(--f->sp)->type = T_STRING;
		str_ref(f->sp->u.string = str_new(p, (long) strlen(p)));
		i_set_rllevel(f, u);
	    }
	    break;

	case I_RLIMITS:
	    if (f->sp[1].type != T_INT) {
		error("Bad rlimits depth type");
	    }
	    if (f->sp->type != T_INT) {
		error("Bad rlimits ticks type");
	    }
	    newdepth = f->sp[1].u.number;
	    newticks = f->sp->u.number;
	    if (!FETCH1U(pc)) {
		/* runtime check */
		i_check_rlimits(f);
	    } else {
		/* pop limits */
		f->sp += 2;
	    }

	    i_set_rlimits(f, newdepth, newticks);
	    i_interpret(f, pc);
	    pc = f->pc;
	    i_set_rllevel(f, -1);
	    break;

	case I_RETURN:
	    return;
	}

	if (instr & I_POP_BIT) {
	    /* pop the result of the last operation (never an lvalue) */
	    i_del_value(f->sp++);
	}
    }
}

/*
 * NAME:	interpret->funcall()
 * DESCRIPTION:	Call a function in an object. The arguments must be on the
 *		stack already.
 */
void i_funcall(prev_f, obj, p_ctrli, funci, nargs)
register frame *prev_f;
register object *obj;
register int p_ctrli, nargs;
int funci;
{
    register char *pc;
    register unsigned short n;
    frame f;
    value val;

    f.prev = prev_f;
    if (prev_f->obj == (object *) NULL) {
	/*
	 * top level call
	 */
	f.obj = obj;
	f.ctrl = obj->ctrl;
	f.data = o_dataspace(obj);
	f.depth = 0;
	f.external = TRUE;
    } else if (obj != (object *) NULL) {
	/*
	 * call_other
	 */
	f.obj = obj;
	f.ctrl = obj->ctrl;
	f.data = o_dataspace(obj);
	f.depth = prev_f->depth + 1;
	f.external = TRUE;
    } else {
	/*
	 * local function call
	 */
	f.obj = prev_f->obj;
	f.ctrl = prev_f->ctrl;
	f.data = prev_f->data;
	f.depth = prev_f->depth + 1;
	f.external = FALSE;
    }
    f.maxdepth = prev_f->maxdepth;
    f.nodepth = prev_f->nodepth;
    if (f.depth >= f.maxdepth && !f.nodepth) {
	error("Stack overflow");
    }
    f.ticks = prev_f->ticks;
    f.noticks = prev_f->noticks;
    if (f.ticks < 100) {
	if (f.noticks) {
	    f.ticks = 0x7fffffff;
	} else {
	    error("Out of ticks");
	}
    }
    f.rli = prev_f->rli;

    /* set the program control block */
    f.foffset = f.ctrl->inherits[p_ctrli].funcoffset;
    f.p_ctrl = o_control(f.ctrl->inherits[p_ctrli].obj);
    f.p_index = p_ctrli + 1;

    /* get the function */
    f.func = &d_get_funcdefs(f.p_ctrl)[funci];
    if (f.func->class & C_UNDEFINED) {
	error("Undefined function %s",
	      d_get_strconst(f.p_ctrl, f.func->inherit, f.func->index)->text);
    }

    pc = d_get_prog(f.p_ctrl) + f.func->offset;
    if (f.func->class & C_TYPECHECKED) {
	/* typecheck arguments */
	i_typecheck(prev_f, d_get_strconst(f.p_ctrl, f.func->inherit,
					   f.func->index)->text,
		    "function", pc, nargs, FALSE);
    }

    /* handle arguments */
    n = PROTO_NARGS(pc);
    pc = PROTO_ARGS(pc);
    if (n > 0 && (UCHAR(pc[n - 1]) & T_ELLIPSIS)) {
	register value *v;
	array *a;

	if (nargs >= n) {
	    /* put additional arguments in array */
	    nargs -= n - 1;
	    a = arr_new(f.data, (long) nargs);
	    v = a->elts + nargs;
	    do {
		*--v = *prev_f->sp++;
	    } while (--nargs > 0);
	    d_ref_imports(a);
	    nargs = n;
	    pc += nargs;
	} else {
	    /* make empty arguments array, and optionally push zeros */
	    i_grow_stack(prev_f, n - nargs);
	    pc += nargs;
	    while (++nargs < n) {
		switch (FETCH1U(pc)) {
		case T_INT:
		    *--prev_f->sp = zero_int;
		    break;

		case T_FLOAT:
		    *--prev_f->sp = zero_float;
		    break;

		default:
		    *--prev_f->sp = nil_value;
		    break;
		}
	    }
	    pc++;
	    a = arr_new(f.data, 0L);
	}
	(--prev_f->sp)->type = T_ARRAY;
	arr_ref(prev_f->sp->u.array = a);
    } else if (nargs > n) {
	/* pop superfluous arguments */
	i_pop(prev_f, nargs - n);
	nargs = n;
	pc += nargs;
    } else {
	pc += nargs;
	if (nargs < n) {
	    /* add missing arguments */
	    i_grow_stack(prev_f, n - nargs);
	    do {
		switch (FETCH1U(pc)) {
		case T_INT:
		    *--prev_f->sp = zero_int;
		    break;

		case T_FLOAT:
		    *--prev_f->sp = zero_float;
			break;

		default:
		    *--prev_f->sp = nil_value;
		    break;
		}
	    } while (++nargs < n);
	}
    }
    f.argp = prev_f->sp;
    cframe = &f;
    f.nargs = nargs;

    /* create new local stack */
    f.prev_lip = prev_f->lip;
    FETCH2U(pc, n);
    f.stack = f.lip = ALLOCA(value, n + MIN_STACK + EXTRA_STACK);
    f.fp = f.sp = f.stack + n + MIN_STACK + EXTRA_STACK;
    f.sos = TRUE;

    /* initialize local variables */
    n = FETCH1U(pc);
# ifdef DEBUG
    nargs = n;
# endif
    if (n > 0) {
	do {
	    *--f.sp = nil_value;
	} while (--n > 0);
    }

    /* execute code */
    i_add_ticks(&f, 10);
    d_get_funcalls(f.ctrl);	/* make sure they are available */
    if (f.func->class & C_COMPILED) {
	Uint l;

	/* compiled function */
	(*pcfunctions[FETCH3U(pc, l)])(&f);
    } else {
	/* interpreted function */
	f.prog = pc += 2;
	i_interpret(&f, pc);
    }

    /* clean up stack, move return value to outer stackframe */
    val = *f.sp++;
# ifdef DEBUG
    if (f.sp != f.fp - nargs || f.lip != f.stack) {
	fatal("bad stack pointer after function call");
    }
# endif
    i_pop(&f, f.fp - f.sp);
    if (f.sos) {
	/* still alloca'd */
	AFREE(f.stack);
    } else {
	/* extended and malloced */
	FREE(f.stack);
    }
    prev_f->ticks = f.ticks;
    cframe = prev_f;
    i_pop(prev_f, f.nargs);
    *--prev_f->sp = val;
}

/*
 * NAME:	interpret->call()
 * DESCRIPTION:	Attempt to call a function in an object. Return TRUE if
 *		the call succeeded.
 */
bool i_call(f, obj, func, len, call_static, nargs)
frame *f;
object *obj;
char *func;
unsigned int len;
int call_static;
int nargs;
{
    register dsymbol *symb;
    register dfuncdef *fdef;
    register control *ctrl;

    ctrl = o_control(obj);
    if (!(obj->flags & O_CREATED)) {
	/*
	 * initialize the object
	 */
	obj->flags |= O_CREATED;
	if (i_call(f, obj, creator, clen, TRUE, 0)) {
	    i_del_value(f->sp++);
	}
    }

    /* find the function in the symbol table */
    symb = ctrl_symb(ctrl, func, len);
    if (symb == (dsymbol *) NULL) {
	/* function doesn't exist in symbol table */
	i_pop(f, nargs);
	return FALSE;
    }

    ctrl = ctrl->inherits[UCHAR(symb->inherit)].obj->ctrl;
    fdef = &d_get_funcdefs(ctrl)[UCHAR(symb->index)];

    /* check if the function can be called */
    if (!call_static && (fdef->class & C_STATIC) && f->obj != obj) {
	i_pop(f, nargs);
	return FALSE;
    }

    /* call the function */
    i_funcall(f, obj, UCHAR(symb->inherit), UCHAR(symb->index), nargs);

    return TRUE;
}

/*
 * NAME:	interpret->line()
 * DESCRIPTION:	return the line number the program counter of the specified
 *		frame is at
 */
static unsigned short i_line(f)
register frame *f;
{
    register char *pc, *numbers;
    register int instr;
    register short offset;
    register unsigned short line, u, sz;

    line = 0;
    pc = f->p_ctrl->prog + f->func->offset;
    pc += PROTO_SIZE(pc) + 3;
    FETCH2U(pc, u);
    numbers = pc + u;

    while (pc < f->pc) {
	instr = FETCH1U(pc);

	offset = instr >> I_LINE_SHIFT;
	if (offset <= 2) {
	    /* simple offset */
	    line += offset;
	} else {
	    offset = FETCH1U(numbers);
	    if (offset >= 128) {
		/* one byte offset */
		line += offset - 128 - 64;
	    } else {
		/* two byte offset */
		line += ((offset << 8) | FETCH1U(numbers)) - 16384;
	    }
	}

	switch (instr & I_INSTR_MASK) {
	case I_INDEX_LVAL:
	    if (instr & I_TYPE_BIT) {
		pc++;
	    }
	    /* fall through */
	case I_PUSH_ZERO:
	case I_PUSH_ONE:
	case I_INDEX:
	case I_FETCH:
	case I_STORE:
	case I_RETURN:
	    break;

	case I_PUSH_LOCAL_LVAL:
	case I_PUSH_GLOBAL_LVAL:
	    if (instr & I_TYPE_BIT) {
		pc++;
	    }
	    /* fall through */
	case I_PUSH_INT1:
	case I_PUSH_STRING:
	case I_PUSH_LOCAL:
	case I_PUSH_GLOBAL:
	case I_SPREAD:
	case I_CAST:
	case I_RLIMITS:
	    pc++;
	    break;

	case I_PUSH_FAR_GLOBAL_LVAL:
	    if (instr & I_TYPE_BIT) {
		pc++;
	    }
	    /* fall through */
	case I_PUSH_NEAR_STRING:
	case I_PUSH_FAR_GLOBAL:
	case I_JUMP:
	case I_JUMP_ZERO:
	case I_JUMP_NONZERO:
	case I_CALL_AFUNC:
	case I_CATCH:
	    pc += 2;
	    break;

	case I_PUSH_FAR_STRING:
	case I_AGGREGATE:
	case I_CALL_DFUNC:
	case I_CALL_FUNC:
	    pc += 3;
	    break;

	case I_PUSH_INT4:
	    pc += 4;
	    break;

	case I_PUSH_FLOAT:
	    pc += 6;
	    break;

	case I_SWITCH:
	    switch (FETCH1U(pc)) {
	    case 0:
		FETCH2U(pc, u);
		sz = FETCH1U(pc);
		pc += 2 + (u - 1) * (sz + 2);
		break;

	    case 1:
		FETCH2U(pc, u);
		sz = FETCH1U(pc);
		pc += 2 + (u - 1) * (2 * sz + 2);
		break;

	    case 2:
		FETCH2U(pc, u);
		pc += 2;
		if (FETCH1U(pc) == 0) {
		    pc += 2;
		    --u;
		}
		pc += (u - 1) * 5;
		break;
	    }
	    break;

	case I_CALL_KFUNC:
	    if (PROTO_CLASS(KFUN(FETCH1U(pc)).proto) & C_VARARGS) {
		pc++;
	    }
	    break;
	}
    }

    return line;
}

/*
 * NAME:	interpret->func_trace()
 * DESCRIPTION:	return the trace of a single function
 */
static array *i_func_trace(f, data)
register frame *f;
dataspace *data;
{
    char buffer[STRINGSZ + 12];
    register value *v;
    register string *str;
    register char *name;
    register unsigned short n;
    register value *args;
    array *a;
    unsigned short max_args;

    max_args = conf_array_size() - 5;

    n = f->nargs;
    args = f->argp + n;
    if (n > max_args) {
	/* unlikely, but possible */
	n = max_args;
    }
    a = arr_new(data, n + 5L);
    v = a->elts;

    /* object name */
    name = o_name(buffer, f->obj);
    v[0].type = T_STRING;
    str = str_new((char *) NULL, strlen(name) + 1L);
    str->text[0] = '/';
    strcpy(str->text + 1, name);
    str_ref(v[0].u.string = str);

    /* program name */
    name = f->p_ctrl->obj->chain.name;
    v[1].type = T_STRING;
    str = str_new((char *) NULL, strlen(name) + 1L);
    str->text[0] = '/';
    strcpy(str->text + 1, name);
    str_ref(v[1].u.string = str);

    /* function name */
    v[2].type = T_STRING;
    str_ref(v[2].u.string = d_get_strconst(f->p_ctrl, f->func->inherit,
					   f->func->index));

    /* line number */
    v[3].type = T_INT;
    if (f->func->class & C_COMPILED) {
	v[3].u.number = 0;
    } else {
	v[3].u.number = i_line(f);
    }

    /* external flag */
    v[4].type = T_INT;
    v[4].u.number = f->external;

    /* arguments */
    v += 5;
    while (n > 0) {
	*v++ = *--args;
	i_ref_value(args);
	--n;
    }
    d_ref_imports(a);

    return a;
}

/*
 * NAME:	interpret->call_tracei()
 * DESCRIPTION:	get the trace of a single function
 */
bool i_call_tracei(ftop, idx, v)
frame *ftop;
Int idx;
value *v;
{
    register frame *f;
    register unsigned short n;

    for (f = ftop, n = 0; f->obj != (object *) NULL; f = f->prev, n++) ;
    if (idx < 0 || idx >= n) {
	return FALSE;
    }

    for (f = ftop, n -= idx + 1; n != 0; f = f->prev, --n) ;
    v->type = T_ARRAY;
    arr_ref(v->u.array = i_func_trace(f, ftop->data));
    return TRUE;
}

/*
 * NAME:	interpret->call_trace()
 * DESCRIPTION:	return the function call trace
 */
array *i_call_trace(ftop)
register frame *ftop;
{
    register frame *f;
    register value *v;
    register unsigned short n;
    array *a;

    for (f = ftop, n = 0; f->obj != (object *) NULL; f = f->prev, n++) ;
    a = arr_new(ftop->data, (long) n);
    i_add_ticks(ftop, 10 * n);
    for (f = ftop, v = a->elts + n; f->obj != (object *) NULL; f = f->prev) {
	(--v)->type = T_ARRAY;
	arr_ref(v->u.array = i_func_trace(f, ftop->data));
    }

    return a;
}

/*
 * NAME:	interpret->call_critical()
 * DESCRIPTION:	Call a function in the driver object at a critical moment.
 *		The function is called with rlimits (-1; -1) and errors
 *		caught.
 */
bool i_call_critical(f, func, narg, flag)
register frame *f;
char *func;
int narg, flag;
{
    bool xnodepth, xnoticks, ok;
    Int xticks;

    xnodepth = f->nodepth;
    xnoticks = f->noticks;
    xticks = f->ticks;
    f->nodepth = TRUE;
    f->noticks = TRUE;

    f->sp += narg;		/* so the error context knows what to pop */
    if (ec_push((flag) ? (ec_ftn) i_catcherr : (ec_ftn) NULL)) {
	ok = FALSE;
    } else {
	f->sp -= narg;	/* recover arguments */
	call_driver_object(f, func, narg);
	ec_pop();
	ok = TRUE;
    }

    f->nodepth = xnodepth;
    f->noticks = xnoticks;
    f->ticks = xticks;

    return ok;
}

/*
 * NAME:	interpret->runtime_error()
 * DESCRIPTION:	handle a runtime error
 */
void i_runtime_error(f, depth)
register frame *f;
Int depth;
{
    char *err;

    err = errormesg();
    (--f->sp)->type = T_STRING;
    str_ref(f->sp->u.string = str_new(err, (long) strlen(err)));
    (--f->sp)->type = T_INT;
    f->sp->u.number = depth;
    (--f->sp)->type = T_INT;
    f->sp->u.number = i_get_ticks(f);
    if (!i_call_critical(f, "runtime_error", 3, FALSE)) {
	message("Error within runtime_error:\012");	/* LF */
	message((char *) NULL);
    } else {
	i_del_value(f->sp++);
    }
}

/*
 * NAME:	interpret->clear()
 * DESCRIPTION:	clean up the interpreter state
 */
void i_clear()
{
    register frame *f;

    f = cframe;
    if (f->stack != stack) {
	FREE(f->stack);
	f->fp = f->sp = stack + MIN_STACK;
	f->stack = f->prev_lip = f->lip = stack;
    }

    f->nodepth = TRUE;
    f->noticks = TRUE;
    f->rli = 0;
}
