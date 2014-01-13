#include "js.h"
#include "jsparse.h"
#include "jscompile.h"
#include "jsstate.h"

#define cexp js_cexp /* collision with math.h */

#define JF js_State *J, js_Function *F

static void cfunbody(JF, js_Ast *name, js_Ast *params, js_Ast *body);
static void cexp(JF, js_Ast *exp);
static void cstmlist(JF, js_Ast *list);

static int listlen(js_Ast *list)
{
	int n = 0;
	while (list) {
		n++;
		list = list->b;
	}
	return n;
}

static js_Function *newfun(js_State *J, js_Ast *name, js_Ast *params, js_Ast *body)
{
	js_Function *F = malloc(sizeof *F);
	memset(F, 0, sizeof *F);

	F->name = name ? name->string : "<anonymous>";
	F->numparams = listlen(params);

	F->codecap = 256;
	F->codelen = 0;
	F->code = malloc(F->codecap * sizeof *F->code);

	F->next = J->fun;
	J->fun = F;

	cfunbody(J, F, name, params, body);

	return F;
}

static void freefun(js_State *J, js_Function *F)
{
//	int i;
//	for (i = 0; i < F->funlen; i++)
//		freefun(J, F->funlist[i]);
	free(F->funlist);
	free(F->numlist);
	free(F->strlist);
	free(F->code);
	free(F);
}

/* Emit opcodes, constants and jumps */

static void emit(JF, int value)
{
	if (F->codelen >= F->codecap) {
		F->codecap *= 2;
		F->code = realloc(F->code, F->codecap * sizeof *F->code);
	}
	F->code[F->codelen++] = value;
}

static int addfunction(JF, js_Function *value)
{
	if (F->funlen >= F->funcap) {
		F->funcap = F->funcap ? F->funcap * 2 : 16;
		F->funlist = realloc(F->funlist, F->funcap * sizeof *F->funlist);
	}
	F->funlist[F->funlen] = value;
	return F->funlen++;
}

static int addnumber(JF, double value)
{
	int i;
	for (i = 0; i < F->numlen; i++)
		if (F->numlist[i] == value)
			return i;
	if (F->numlen >= F->numcap) {
		F->numcap = F->numcap ? F->numcap * 2 : 16;
		F->numlist = realloc(F->numlist, F->numcap * sizeof *F->numlist);
	}
	F->numlist[F->numlen] = value;
	return F->numlen++;
}

static int addstring(JF, const char *value)
{
	int i;
	for (i = 0; i < F->strlen; i++)
		if (!strcmp(F->strlist[i], value))
			return i;
	if (F->strlen >= F->strcap) {
		F->strcap = F->strcap ? F->strcap * 2 : 16;
		F->strlist = realloc(F->strlist, F->strcap * sizeof *F->strlist);
	}
	F->strlist[F->strlen] = value;
	return F->strlen++;
}

static void emitfunction(JF, int opcode, js_Function *fun)
{
	emit(J, F, opcode);
	emit(J, F, addfunction(J, F, fun));
}

static void emitnumber(JF, int opcode, double num)
{
	emit(J, F, opcode);
	emit(J, F, addnumber(J, F, num));
}

static void emitstring(JF, int opcode, const char *str)
{
	emit(J, F, opcode);
	emit(J, F, addstring(J, F, str));
}

static int here(JF)
{
	return F->codelen;
}

static int jump(JF, int opcode)
{
	int inst = F->codelen + 1;
	emit(J, F, opcode);
	emit(J, F, 0);
	return inst;
}

static void jumpto(JF, int opcode, int dest)
{
	emit(J, F, opcode);
	emit(J, F, dest);
}

static void label(JF, int inst)
{
	F->code[inst] = F->codelen;
}

/* Expressions */

static void unary(JF, js_Ast *exp, int opcode)
{
	cexp(J, F, exp->a);
	emit(J, F, opcode);
}

static void binary(JF, js_Ast *exp, int opcode)
{
	cexp(J, F, exp->a);
	cexp(J, F, exp->b);
	emit(J, F, opcode);
}

static void carray(JF, js_Ast *list)
{
	int i = 0;
	while (list) {
		cexp(J, F, list->a);
		emit(J, F, OP_ARRAYPUT);
		emit(J, F, i++);
		list = list->b;
	}
}

static void cobject(JF, js_Ast *list)
{
	while (list) {
		js_Ast *kv = list->a;
		if (kv->type == EXP_PROP_VAL) {
			js_Ast *prop = kv->a;
			cexp(J, F, kv->b);
			if (prop->type == AST_IDENTIFIER || prop->type == AST_STRING)
				emitstring(J, F, OP_OBJECTPUT, prop->string);
			else if (prop->type == AST_STRING)
				emitstring(J, F, OP_OBJECTPUT, prop->string);
			else if (prop->type == AST_NUMBER)
				emitnumber(J, F, OP_OBJECTPUT, prop->number);
			else
				jsC_error(J, list, "illegal property name in object initializer");
		}
		// TODO: set, get
		list = list->b;
	}
}

static int cargs(JF, js_Ast *list)
{
	int n = 0;
	while (list) {
		cexp(J, F, list->a);
		list = list->b;
		n++;
	}
	return n;
}

static void clval(JF, js_Ast *exp)
{
	switch (exp->type) {
	case AST_IDENTIFIER:
		emitstring(J, F, OP_AVAR, exp->string);
		break;
	case EXP_INDEX:
		cexp(J, F, exp->a);
		cexp(J, F, exp->b);
		emit(J, F, OP_AINDEX);
		break;
	case EXP_MEMBER:
		cexp(J, F, exp->a);
		emitstring(J, F, OP_AMEMBER, exp->b->string);
		break;
	case EXP_CALL:
		/* host functions may return an assignable l-value */
		cexp(J, F, exp);
		break;
	default:
		jsC_error(J, exp, "invalid l-value in assignment");
		break;
	}
}

static void assignop(JF, js_Ast *exp, int opcode)
{
	clval(J, F, exp->a);
	emit(J, F, OP_LOAD);
	cexp(J, F, exp->b);
	emit(J, F, opcode);
	emit(J, F, OP_STORE);
}

static void cvarinit(JF, js_Ast *list)
{
	while (list) {
		js_Ast *var = list->a;
		if (var->b) {
			cexp(J, F, var->b);
			emitstring(J, F, OP_AVAR, var->a->string);
			emit(J, F, OP_STORE);
			emit(J, F, OP_POP);
		}
		list = list->b;
	}
}

static void ccall(JF, js_Ast *fun, js_Ast *args)
{
	int n;
	switch (fun->type) {
	case EXP_INDEX:
		cexp(J, F, fun->a);
		emit(J, F, OP_DUP);
		cexp(J, F, fun->b);
		emit(J, F, OP_LOADINDEX);
		break;
	case EXP_MEMBER:
		cexp(J, F, fun->a);
		emit(J, F, OP_DUP);
		emitstring(J, F, OP_LOADMEMBER, fun->b->string);
		break;
	default:
		emit(J, F, OP_THIS);
		cexp(J, F, fun);
		break;
	}
	n = cargs(J, F, args);
	emit(J, F, OP_CALL);
	emit(J, F, n);
}

static void cexp(JF, js_Ast *exp)
{
	int then, end;
	int n;

	switch (exp->type) {
	case AST_IDENTIFIER: emitstring(J, F, OP_LOADVAR, exp->string); break;
	case AST_NUMBER: emitnumber(J, F, OP_NUMBER, exp->number); break;
	case AST_STRING: emitstring(J, F, OP_STRING, exp->string); break;
	case EXP_UNDEF: emit(J, F, OP_UNDEF); break;
	case EXP_NULL: emit(J, F, OP_NULL); break;
	case EXP_TRUE: emit(J, F, OP_TRUE); break;
	case EXP_FALSE: emit(J, F, OP_FALSE); break;
	case EXP_THIS: emit(J, F, OP_THIS); break;

	case EXP_OBJECT:
		emit(J, F, OP_NEWOBJECT);
		cobject(J, F, exp->a);
		break;

	case EXP_ARRAY:
		emit(J, F, OP_NEWARRAY);
		carray(J, F, exp->a);
		break;

	case EXP_INDEX:
		cexp(J, F, exp->a);
		cexp(J, F, exp->b);
		emit(J, F, OP_LOADINDEX);
		break;

	case EXP_MEMBER:
		cexp(J, F, exp->a);
		emitstring(J, F, OP_LOADMEMBER, exp->b->string);
		break;

	case EXP_CALL:
		ccall(J, F, exp->a, exp->b);
		break;

	case EXP_NEW:
		cexp(J, F, exp->a);
		n = cargs(J, F, exp->b);
		emit(J, F, OP_NEW);
		emit(J, F, n);
		break;

	case EXP_FUN:
		emitfunction(J, F, OP_CLOSURE, newfun(J, exp->a, exp->b, exp->c));
		break;

	case EXP_DELETE:
		clval(J, F, exp->a);
		emit(J, F, OP_DELETE);
		break;

	case EXP_VOID:
		cexp(J, F, exp->a);
		emit(J, F, OP_POP);
		emit(J, F, OP_UNDEF);
		break;

	case EXP_TYPEOF: unary(J, F, exp, OP_TYPEOF); break;
	case EXP_POS: unary(J, F, exp, OP_POS); break;
	case EXP_NEG: unary(J, F, exp, OP_NEG); break;
	case EXP_BITNOT: unary(J, F, exp, OP_BITNOT); break;
	case EXP_LOGNOT: unary(J, F, exp, OP_LOGNOT); break;

	case EXP_PREINC: clval(J, F, exp->a); emit(J, F, OP_PREINC); break;
	case EXP_PREDEC: clval(J, F, exp->a); emit(J, F, OP_PREDEC); break;
	case EXP_POSTINC: clval(J, F, exp->a); emit(J, F, OP_POSTINC); break;
	case EXP_POSTDEC: clval(J, F, exp->a); emit(J, F, OP_POSTDEC); break;

	case EXP_BITOR: binary(J, F, exp, OP_BITOR); break;
	case EXP_BITXOR: binary(J, F, exp, OP_BITXOR); break;
	case EXP_BITAND: binary(J, F, exp, OP_BITAND); break;
	case EXP_EQ: binary(J, F, exp, OP_EQ); break;
	case EXP_NE: binary(J, F, exp, OP_NE); break;
	case EXP_EQ3: binary(J, F, exp, OP_EQ3); break;
	case EXP_NE3: binary(J, F, exp, OP_NE3); break;
	case EXP_LT: binary(J, F, exp, OP_LT); break;
	case EXP_GT: binary(J, F, exp, OP_GT); break;
	case EXP_LE: binary(J, F, exp, OP_LE); break;
	case EXP_GE: binary(J, F, exp, OP_GE); break;
	case EXP_INSTANCEOF: binary(J, F, exp, OP_INSTANCEOF); break;
	case EXP_IN: binary(J, F, exp, OP_IN); break;
	case EXP_SHL: binary(J, F, exp, OP_SHL); break;
	case EXP_SHR: binary(J, F, exp, OP_SHR); break;
	case EXP_USHR: binary(J, F, exp, OP_USHR); break;
	case EXP_ADD: binary(J, F, exp, OP_ADD); break;
	case EXP_SUB: binary(J, F, exp, OP_SUB); break;
	case EXP_MUL: binary(J, F, exp, OP_MUL); break;
	case EXP_DIV: binary(J, F, exp, OP_DIV); break;
	case EXP_MOD: binary(J, F, exp, OP_MOD); break;

	case EXP_ASS:
		clval(J, F, exp->a);
		cexp(J, F, exp->b);
		emit(J, F, OP_STORE);
		break;

	case EXP_ASS_MUL: assignop(J, F, exp, OP_MUL); break;
	case EXP_ASS_DIV: assignop(J, F, exp, OP_DIV); break;
	case EXP_ASS_MOD: assignop(J, F, exp, OP_MOD); break;
	case EXP_ASS_ADD: assignop(J, F, exp, OP_ADD); break;
	case EXP_ASS_SUB: assignop(J, F, exp, OP_SUB); break;
	case EXP_ASS_SHL: assignop(J, F, exp, OP_SHL); break;
	case EXP_ASS_SHR: assignop(J, F, exp, OP_SHR); break;
	case EXP_ASS_USHR: assignop(J, F, exp, OP_USHR); break;
	case EXP_ASS_BITAND: assignop(J, F, exp, OP_BITAND); break;
	case EXP_ASS_BITXOR: assignop(J, F, exp, OP_BITXOR); break;
	case EXP_ASS_BITOR: assignop(J, F, exp, OP_BITOR); break;

	case EXP_COMMA:
		cexp(J, F, exp->a);
		emit(J, F, OP_POP);
		cexp(J, F, exp->b);
		break;

	case EXP_LOGOR:
		/* if a == true then a else b */
		cexp(J, F, exp->a);
		emit(J, F, OP_DUP);
		end = jump(J, F, OP_JTRUE);
		emit(J, F, OP_POP);
		cexp(J, F, exp->b);
		label(J, F, end);
		break;

	case EXP_LOGAND:
		/* if a == false then a else b */
		cexp(J, F, exp->a);
		emit(J, F, OP_DUP);
		end = jump(J, F, OP_JFALSE);
		emit(J, F, OP_POP);
		cexp(J, F, exp->b);
		label(J, F, end);
		break;

	case EXP_COND:
		cexp(J, F, exp->a);
		then = jump(J, F, OP_JTRUE);
		cexp(J, F, exp->c);
		end = jump(J, F, OP_JUMP);
		label(J, F, then);
		cexp(J, F, exp->b);
		label(J, F, end);
		break;

	default:
		jsC_error(J, exp, "unknown expression");
	}
}

/* Statements */

static void cstm(JF, js_Ast *stm)
{
	int loop, then, end;

	switch (stm->type) {
	case AST_FUNDEC:
		break;

	case STM_BLOCK:
		cstmlist(J, F, stm->a);
		break;

	case STM_NOP:
		break;

	case STM_VAR:
		cvarinit(J, F, stm->a);
		break;

	case STM_IF:
		if (stm->c) {
			cexp(J, F, stm->a);
			then = jump(J, F, OP_JTRUE);
			cstm(J, F, stm->c);
			end = jump(J, F, OP_JUMP);
			label(J, F, then);
			cstm(J, F, stm->b);
			label(J, F, end);
		} else {
			cexp(J, F, stm->a);
			end = jump(J, F, OP_JFALSE);
			cstm(J, F, stm->b);
			label(J, F, end);
		}
		break;

	case STM_WHILE:
		loop = here(J, F);
		cexp(J, F, stm->a);
		end = jump(J, F, OP_JFALSE);
		cstm(J, F, stm->b);
		jumpto(J, F, OP_JUMP, loop);
		label(J, F, end);
		break;

	case STM_DO:
		loop = here(J, F);
		cstm(J, F, stm->a);
		cexp(J, F, stm->b);
		jumpto(J, F, OP_JTRUE, loop);
		break;

	// for
	// for-in

	case STM_RETURN:
		if (stm->a)
			cexp(J, F, stm->a);
		else
			emit(J, F, OP_UNDEF);
		emit(J, F, OP_RETURN);
		break;

	case STM_WITH:
		cexp(J, F, stm->a);
		emit(J, F, OP_WITH);
		cstm(J, F, stm->b);
		emit(J, F, OP_ENDWITH);
		break;

	// switch
	// throw, try
	// label

	case STM_THROW:
		cexp(J, F, stm->a);
		emit(J, F, OP_THROW);
		break;

	case STM_DEBUGGER:
		emit(J, F, OP_DEBUGGER);
		break;

	default:
		cexp(J, F, stm);
		emit(J, F, OP_POP);
		break;
	}
}

static void cstmlist(JF, js_Ast *list)
{
	while (list) {
		cstm(J, F, list->a);
		list = list->b;
	}
}

/* Declarations and programs */

static void cfundecs(JF, js_Ast *list)
{
	while (list) {
		js_Ast *stm = list->a;
		if (stm->type == AST_FUNDEC) {
			emitfunction(J, F, OP_CLOSURE, newfun(J, stm->a, stm->b, stm->c));
			emitstring(J, F, OP_FUNDEC, stm->a->string);
		}
		list = list->b;
	}
}

static void cvardecs(JF, js_Ast *node)
{
	if (node->type == EXP_VAR) {
		emitstring(J, F, OP_VARDEC, node->a->string);
	} else if (node->type != EXP_FUN && node->type != AST_FUNDEC) {
		if (node->a) cvardecs(J, F, node->a);
		if (node->b) cvardecs(J, F, node->b);
		if (node->c) cvardecs(J, F, node->c);
		if (node->d) cvardecs(J, F, node->d);
	}
}

static void cfunbody(JF, js_Ast *name, js_Ast *params, js_Ast *body)
{
	if (name) {
		emitfunction(J, F, OP_CLOSURE, F);
		emitstring(J, F, OP_FUNDEC, name->string);
	}

	if (body) {
		cfundecs(J, F, body);
		cvardecs(J, F, body);
		cstmlist(J, F, body);
	}

	if (F->codelen == 0 || F->code[F->codelen - 1] != OP_RETURN) {
		emit(J, F, OP_UNDEF);
		emit(J, F, OP_RETURN);
	}
}

int jsC_error(js_State *J, js_Ast *node, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s:%d: error: ", J->filename, node->line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");

	longjmp(J->jb, 1);
	return 0;
}

void jsC_freecompile(js_State *J)
{
	js_Function *F = J->fun;
	while (F) {
		js_Function *next = F->next;
		freefun(J, F);
		F = next;
	}
	J->fun = NULL;
}

js_Function *jsC_compile(js_State *J, js_Ast *prog)
{
	js_Function *F;

	if (setjmp(J->jb)) {
		jsC_freecompile(J);
		return NULL;
	}

	F = newfun(J, NULL, NULL, prog);

	J->fun = NULL;
	return F;
}