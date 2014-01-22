#include "jsi.h"
#include "jslex.h"
#include "jsparse.h"

#define nelem(a) (sizeof (a) / sizeof (a)[0])

#define LIST(h)		jsP_newnode(J, AST_LIST, h, 0, 0, 0)

#define EXP0(x)		jsP_newnode(J, EXP_ ## x, 0, 0, 0, 0)
#define EXP1(x,a)	jsP_newnode(J, EXP_ ## x, a, 0, 0, 0)
#define EXP2(x,a,b)	jsP_newnode(J, EXP_ ## x, a, b, 0, 0)
#define EXP3(x,a,b,c)	jsP_newnode(J, EXP_ ## x, a, b, c, 0)

#define STM0(x)		jsP_newnode(J, STM_ ## x, 0, 0, 0, 0)
#define STM1(x,a)	jsP_newnode(J, STM_ ## x, a, 0, 0, 0)
#define STM2(x,a,b)	jsP_newnode(J, STM_ ## x, a, b, 0, 0)
#define STM3(x,a,b,c)	jsP_newnode(J, STM_ ## x, a, b, c, 0)
#define STM4(x,a,b,c,d)	jsP_newnode(J, STM_ ## x, a, b, c, d)

#define TOKSTR		jsY_tokenstring(J->lookahead)

static js_Ast *expression(js_State *J, int notin);
static js_Ast *assignment(js_State *J, int notin);
static js_Ast *memberexp(js_State *J);
static js_Ast *statement(js_State *J);
static js_Ast *funbody(js_State *J);

JS_NORETURN static void jsP_error(js_State *J, const char *fmt, ...) JS_PRINTFLIKE(2,3);

static void jsP_error(js_State *J, const char *fmt, ...)
{
	va_list ap;
	char buf[512];
	char msgbuf[256];

	va_start(ap, fmt);
	vsnprintf(msgbuf, 256, fmt, ap);
	va_end(ap);

	snprintf(buf, 256, "%s:%d: ", J->filename, J->lexline);
	strcat(buf, msgbuf);

	js_newsyntaxerror(J, buf);
	js_throw(J);
}

static void jsP_warning(js_State *J, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "%s:%d: warning: ", J->filename, J->lexline);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static js_Ast *jsP_newnode(js_State *J, int type, js_Ast *a, js_Ast *b, js_Ast *c, js_Ast *d)
{
	js_Ast *node = malloc(sizeof(js_Ast));

	node->type = type;
	node->line = J->lexline;
	node->a = a;
	node->b = b;
	node->c = c;
	node->d = d;
	node->number = 0;
	node->string = NULL;
	node->jumps = NULL;

	node->parent = NULL;
	if (a) a->parent = node;
	if (b) b->parent = node;
	if (c) c->parent = node;
	if (d) d->parent = node;

	node->gcnext = J->gcast;
	J->gcast = node;

	return node;
}

static js_Ast *jsP_list(js_Ast *head)
{
	/* set parent pointers in list nodes */
	js_Ast *prev = head, *node = head->b;
	while (node) {
		node->parent = prev;
		prev = node;
		node = node->b;
	}
	return head;
}

static js_Ast *jsP_newstrnode(js_State *J, int type, const char *s)
{
	js_Ast *node = jsP_newnode(J, type, 0, 0, 0, 0);
	node->string = s;
	return node;
}

static js_Ast *jsP_newnumnode(js_State *J, int type, double n)
{
	js_Ast *node = jsP_newnode(J, type, 0, 0, 0, 0);
	node->number = n;
	return node;
}

static void jsP_freejumps(js_State *J, js_JumpList *node)
{
	while (node) {
		js_JumpList *next = node->next;
		free(node);
		node = next;
	}
}

void jsP_freeparse(js_State *J)
{
	js_Ast *node = J->gcast;
	while (node) {
		js_Ast *next = node->gcnext;
		jsP_freejumps(J, node->jumps);
		free(node);
		node = next;
	}
	J->gcast = NULL;
}

/* Lookahead */

static inline void next(js_State *J)
{
	J->lookahead = jsY_lex(J);
}

static inline int accept(js_State *J, int t)
{
	if (J->lookahead == t) {
		next(J);
		return 1;
	}
	return 0;
}

static inline void expect(js_State *J, int t)
{
	if (accept(J, t))
		return;
	jsP_error(J, "unexpected token: %s (expected %s)", TOKSTR, jsY_tokenstring(t));
}

static void semicolon(js_State *J)
{
	if (J->lookahead == ';') {
		next(J);
		return;
	}
	if (J->newline || J->lookahead == '}' || J->lookahead == 0)
		return;
	jsP_error(J, "unexpected token: %s (expected ';')", TOKSTR);
}

/* Literals */

static const char *futurewords[] = {
	"class", "const", "enum", "export", "extends", "import", "super",
};

static const char *strictfuturewords[] = {
	"implements", "interface", "let", "package", "private", "protected",
	"public", "static", "yield",
};

static void checkfutureword(js_State *J, const char *s)
{
	if (jsY_findword(s, futurewords, nelem(futurewords)) >= 0)
		jsP_error(J, "'%s' is a future reserved word", s);
	if (J->strict && jsY_findword(s, strictfuturewords, nelem(strictfuturewords)) >= 0)
		jsP_error(J, "'%s' is a strict mode future reserved word", s);
}

static js_Ast *identifier(js_State *J)
{
	js_Ast *a;
	if (J->lookahead == TK_IDENTIFIER) {
		checkfutureword(J, J->text);
		a = jsP_newstrnode(J, AST_IDENTIFIER, J->text);
		next(J);
		return a;
	}
	jsP_error(J, "unexpected token: %s (expected identifier)", TOKSTR);
	return NULL;
}

static js_Ast *identifieropt(js_State *J)
{
	if (J->lookahead == TK_IDENTIFIER)
		return identifier(J);
	return NULL;
}

static js_Ast *identifiername(js_State *J)
{
	if (J->lookahead == TK_IDENTIFIER || J->lookahead >= TK_BREAK) {
		js_Ast *a = jsP_newstrnode(J, AST_IDENTIFIER, J->text);
		next(J);
		return a;
	}
	jsP_error(J, "unexpected token: %s (expected identifier or keyword)", TOKSTR);
	return NULL;
}

static js_Ast *arrayelement(js_State *J)
{
	if (J->lookahead == ',')
		return EXP0(UNDEF);
	return assignment(J, 0);
}

static js_Ast *arrayliteral(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == ']')
		return NULL;
	head = tail = LIST(arrayelement(J));
	while (accept(J, ',')) {
		if (J->lookahead != ']')
			tail = tail->b = LIST(arrayelement(J));
	}
	return jsP_list(head);
}

static js_Ast *propname(js_State *J)
{
	js_Ast *name;
	if (J->lookahead == TK_NUMBER) {
		name = jsP_newnumnode(J, AST_NUMBER, J->number);
		next(J);
	} else if (J->lookahead == TK_STRING) {
		name = jsP_newstrnode(J, AST_STRING, J->text);
		next(J);
	} else {
		name = identifiername(J);
	}
	return name;
}

static js_Ast *propassign(js_State *J)
{
	js_Ast *name, *value, *arg, *body;

	name = propname(J);

	if (J->lookahead != ':' && name->type == AST_IDENTIFIER) {
		if (!strcmp(name->string, "get")) {
			name = propname(J);
			expect(J, '(');
			expect(J, ')');
			body = funbody(J);
			return EXP2(PROP_GET, name, body);
		}
		if (!strcmp(name->string, "set")) {
			name = propname(J);
			expect(J, '(');
			arg = identifier(J);
			expect(J, ')');
			body = funbody(J);
			return EXP3(PROP_SET, name, arg, body);
		}
	}

	expect(J, ':');
	value = assignment(J, 0);
	return EXP2(PROP_VAL, name, value);
}

static js_Ast *objectliteral(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == '}')
		return NULL;
	head = tail = LIST(propassign(J));
	while (accept(J, ',')) {
		if (J->lookahead == '}')
			break;
		tail = tail->b = LIST(propassign(J));
	}
	return jsP_list(head);
}

/* Functions */

static js_Ast *parameters(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == ')')
		return NULL;
	head = tail = LIST(identifier(J));
	while (accept(J, ',')) {
		tail = tail->b = LIST(identifier(J));
	}
	return jsP_list(head);
}

static js_Ast *fundec(js_State *J)
{
	js_Ast *a, *b, *c;
	a = identifier(J);
	expect(J, '(');
	b = parameters(J);
	expect(J, ')');
	c = funbody(J);
	return jsP_newnode(J, AST_FUNDEC, a, b, c, 0);
}

static js_Ast *funstm(js_State *J)
{
	js_Ast *a, *b, *c;
	a = identifier(J);
	expect(J, '(');
	b = parameters(J);
	expect(J, ')');
	c = funbody(J);
	/* rewrite function statement as "var X = function X() {}" */
	return STM1(VAR, LIST(EXP2(VAR, a, EXP3(FUN, a, b, c))));
}

static js_Ast *funexp(js_State *J)
{
	js_Ast *a, *b, *c;
	a = identifieropt(J);
	expect(J, '(');
	b = parameters(J);
	expect(J, ')');
	c = funbody(J);
	return EXP3(FUN, a, b, c);
}

/* Expressions */

static js_Ast *primary(js_State *J)
{
	js_Ast *a;

	if (J->lookahead == TK_IDENTIFIER) {
		a = jsP_newstrnode(J, AST_IDENTIFIER, J->text);
		next(J);
		return a;
	}
	if (J->lookahead == TK_STRING) {
		a = jsP_newstrnode(J, AST_STRING, J->text);
		next(J);
		return a;
	}
	if (J->lookahead == TK_REGEXP) {
		a = jsP_newstrnode(J, AST_REGEXP, J->text);
		a->number = J->number;
		next(J);
		return a;
	}
	if (J->lookahead == TK_NUMBER) {
		a = jsP_newnumnode(J, AST_NUMBER, J->number);
		next(J);
		return a;
	}

	if (accept(J, TK_THIS)) return EXP0(THIS);
	if (accept(J, TK_NULL)) return EXP0(NULL);
	if (accept(J, TK_TRUE)) return EXP0(TRUE);
	if (accept(J, TK_FALSE)) return EXP0(FALSE);
	if (accept(J, '{')) { a = EXP1(OBJECT, objectliteral(J)); expect(J, '}'); return a; }
	if (accept(J, '[')) { a = EXP1(ARRAY, arrayliteral(J)); expect(J, ']'); return a; }
	if (accept(J, '(')) { a = expression(J, 0); expect(J, ')'); return a; }

	jsP_error(J, "unexpected token in expression: %s", TOKSTR);
	return NULL;
}

static js_Ast *arguments(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == ')')
		return NULL;
	head = tail = LIST(assignment(J, 0));
	while (accept(J, ',')) {
		tail = tail->b = LIST(assignment(J, 0));
	}
	return jsP_list(head);
}

static js_Ast *newexp(js_State *J)
{
	js_Ast *a, *b;

	if (accept(J, TK_NEW)) {
		a = memberexp(J);
		if (accept(J, '(')) {
			b = arguments(J);
			expect(J, ')');
			return EXP2(NEW, a, b);
		}
		return EXP1(NEW, a);
	}

	if (accept(J, TK_FUNCTION))
		return funexp(J);

	return primary(J);
}

static js_Ast *memberexp(js_State *J)
{
	js_Ast *a = newexp(J);
loop:
	if (accept(J, '.')) { a = EXP2(MEMBER, a, identifiername(J)); goto loop; }
	if (accept(J, '[')) { a = EXP2(INDEX, a, expression(J, 0)); expect(J, ']'); goto loop; }
	return a;
}

static js_Ast *callexp(js_State *J)
{
	js_Ast *a = newexp(J);
loop:
	if (accept(J, '.')) { a = EXP2(MEMBER, a, identifiername(J)); goto loop; }
	if (accept(J, '[')) { a = EXP2(INDEX, a, expression(J, 0)); expect(J, ']'); goto loop; }
	if (accept(J, '(')) { a = EXP2(CALL, a, arguments(J)); expect(J, ')'); goto loop; }
	return a;
}

static js_Ast *postfix(js_State *J)
{
	js_Ast *a = callexp(J);
	if (!J->newline && accept(J, TK_INC)) return EXP1(POSTINC, a);
	if (!J->newline && accept(J, TK_DEC)) return EXP1(POSTDEC, a);
	return a;
}

static js_Ast *unary(js_State *J)
{
	if (accept(J, TK_DELETE)) return EXP1(DELETE, unary(J));
	if (accept(J, TK_VOID)) return EXP1(VOID, unary(J));
	if (accept(J, TK_TYPEOF)) return EXP1(TYPEOF, unary(J));
	if (accept(J, TK_INC)) return EXP1(PREINC, unary(J));
	if (accept(J, TK_DEC)) return EXP1(PREDEC, unary(J));
	if (accept(J, '+')) return EXP1(POS, unary(J));
	if (accept(J, '-')) return EXP1(NEG, unary(J));
	if (accept(J, '~')) return EXP1(BITNOT, unary(J));
	if (accept(J, '!')) return EXP1(LOGNOT, unary(J));
	return postfix(J);
}

static js_Ast *multiplicative(js_State *J)
{
	js_Ast *a = unary(J);
loop:
	if (accept(J, '*')) { a = EXP2(MUL, a, unary(J)); goto loop; }
	if (accept(J, '/')) { a = EXP2(DIV, a, unary(J)); goto loop; }
	if (accept(J, '%')) { a = EXP2(MOD, a, unary(J)); goto loop; }
	return a;
}

static js_Ast *additive(js_State *J)
{
	js_Ast *a = multiplicative(J);
loop:
	if (accept(J, '+')) { a = EXP2(ADD, a, multiplicative(J)); goto loop; }
	if (accept(J, '-')) { a = EXP2(SUB, a, multiplicative(J)); goto loop; }
	return a;
}

static js_Ast *shift(js_State *J)
{
	js_Ast *a = additive(J);
loop:
	if (accept(J, TK_SHL)) { a = EXP2(SHL, a, additive(J)); goto loop; }
	if (accept(J, TK_SHR)) { a = EXP2(SHR, a, additive(J)); goto loop; }
	if (accept(J, TK_USHR)) { a = EXP2(USHR, a, additive(J)); goto loop; }
	return a;
}

static js_Ast *relational(js_State *J, int notin)
{
	js_Ast *a = shift(J);
loop:
	if (accept(J, '<')) { a = EXP2(LT, a, shift(J)); goto loop; }
	if (accept(J, '>')) { a = EXP2(GT, a, shift(J)); goto loop; }
	if (accept(J, TK_LE)) { a = EXP2(LE, a, shift(J)); goto loop; }
	if (accept(J, TK_GE)) { a = EXP2(GE, a, shift(J)); goto loop; }
	if (accept(J, TK_INSTANCEOF)) { a = EXP2(INSTANCEOF, a, shift(J)); goto loop; }
	if (!notin && accept(J, TK_IN)) { a = EXP2(IN, a, shift(J)); goto loop; }
	return a;
}

static js_Ast *equality(js_State *J, int notin)
{
	js_Ast *a = relational(J, notin);
loop:
	if (accept(J, TK_EQ)) { a = EXP2(EQ, a, relational(J, notin)); goto loop; }
	if (accept(J, TK_NE)) { a = EXP2(NE, a, relational(J, notin)); goto loop; }
	if (accept(J, TK_STRICTEQ)) { a = EXP2(STRICTEQ, a, relational(J, notin)); goto loop; }
	if (accept(J, TK_STRICTNE)) { a = EXP2(STRICTNE, a, relational(J, notin)); goto loop; }
	return a;
}

static js_Ast *bitand(js_State *J, int notin)
{
	js_Ast *a = equality(J, notin);
	while (accept(J, '&'))
		a = EXP2(BITAND, a, equality(J, notin));
	return a;
}

static js_Ast *bitxor(js_State *J, int notin)
{
	js_Ast *a = bitand(J, notin);
	while (accept(J, '^'))
		a = EXP2(BITXOR, a, bitand(J, notin));
	return a;
}

static js_Ast *bitor(js_State *J, int notin)
{
	js_Ast *a = bitxor(J, notin);
	while (accept(J, '|'))
		a = EXP2(BITOR, a, bitxor(J, notin));
	return a;
}

static js_Ast *logand(js_State *J, int notin)
{
	js_Ast *a = bitor(J, notin);
	if (accept(J, TK_AND))
		a = EXP2(LOGAND, a, logand(J, notin));
	return a;
}

static js_Ast *logor(js_State *J, int notin)
{
	js_Ast *a = logand(J, notin);
	if (accept(J, TK_OR))
		a = EXP2(LOGOR, a, logor(J, notin));
	return a;
}

static js_Ast *conditional(js_State *J, int notin)
{
	js_Ast *a, *b, *c;
	a = logor(J, notin);
	if (accept(J, '?')) {
		b = assignment(J, 0);
		expect(J, ':');
		c = assignment(J, notin);
		return EXP3(COND, a, b, c);
	}
	return a;
}

static js_Ast *assignment(js_State *J, int notin)
{
	js_Ast *a = conditional(J, notin);
	if (accept(J, '=')) return EXP2(ASS, a, assignment(J, notin));
	if (accept(J, TK_MUL_ASS)) return EXP2(ASS_MUL, a, assignment(J, notin));
	if (accept(J, TK_DIV_ASS)) return EXP2(ASS_DIV, a, assignment(J, notin));
	if (accept(J, TK_MOD_ASS)) return EXP2(ASS_MOD, a, assignment(J, notin));
	if (accept(J, TK_ADD_ASS)) return EXP2(ASS_ADD, a, assignment(J, notin));
	if (accept(J, TK_SUB_ASS)) return EXP2(ASS_SUB, a, assignment(J, notin));
	if (accept(J, TK_SHL_ASS)) return EXP2(ASS_SHL, a, assignment(J, notin));
	if (accept(J, TK_SHR_ASS)) return EXP2(ASS_SHR, a, assignment(J, notin));
	if (accept(J, TK_USHR_ASS)) return EXP2(ASS_USHR, a, assignment(J, notin));
	if (accept(J, TK_AND_ASS)) return EXP2(ASS_BITAND, a, assignment(J, notin));
	if (accept(J, TK_XOR_ASS)) return EXP2(ASS_BITXOR, a, assignment(J, notin));
	if (accept(J, TK_OR_ASS)) return EXP2(ASS_BITOR, a, assignment(J, notin));
	return a;
}

static js_Ast *expression(js_State *J, int notin)
{
	js_Ast *a = assignment(J, notin);
	while (accept(J, ','))
		a = EXP2(COMMA, a, assignment(J, notin));
	return a;
}

/* Statements */

static js_Ast *vardec(js_State *J, int notin)
{
	js_Ast *a = identifier(J);
	if (accept(J, '='))
		return EXP2(VAR, a, assignment(J, notin));
	return EXP1(VAR, a);
}

static js_Ast *vardeclist(js_State *J, int notin)
{
	js_Ast *head, *tail;
	head = tail = LIST(vardec(J, notin));
	while (accept(J, ','))
		tail = tail->b = LIST(vardec(J, notin));
	return jsP_list(head);
}

static js_Ast *statementlist(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == '}' || J->lookahead == TK_CASE || J->lookahead == TK_DEFAULT)
		return NULL;
	head = tail = LIST(statement(J));
	while (J->lookahead != '}' && J->lookahead != TK_CASE && J->lookahead != TK_DEFAULT)
		tail = tail->b = LIST(statement(J));
	return jsP_list(head);
}

static js_Ast *caseclause(js_State *J)
{
	js_Ast *a, *b;

	if (accept(J, TK_CASE)) {
		a = expression(J, 0);
		expect(J, ':');
		b = statementlist(J);
		return STM2(CASE, a, b);
	}

	if (accept(J, TK_DEFAULT)) {
		expect(J, ':');
		a = statementlist(J);
		return STM1(DEFAULT, a);
	}

	jsP_error(J, "unexpected token in switch: %s (expected 'case' or 'default')", TOKSTR);
	return NULL;
}

static js_Ast *caselist(js_State *J)
{
	js_Ast *head, *tail;
	if (J->lookahead == '}')
		return NULL;
	head = tail = LIST(caseclause(J));
	while (J->lookahead != '}')
		tail = tail->b = LIST(caseclause(J));
	return jsP_list(head);
}

static js_Ast *block(js_State *J)
{
	js_Ast *a;
	expect(J, '{');
	a = statementlist(J);
	expect(J, '}');
	return STM1(BLOCK, a);
}

static js_Ast *forexpression(js_State *J, int end)
{
	js_Ast *a = NULL;
	if (J->lookahead != end)
		a = expression(J, 0);
	expect(J, end);
	return a;
}

static js_Ast *forstatement(js_State *J)
{
	js_Ast *a, *b, *c, *d;
	expect(J, '(');
	if (accept(J, TK_VAR)) {
		a = vardeclist(J, 1);
		if (accept(J, ';')) {
			b = forexpression(J, ';');
			c = forexpression(J, ')');
			d = statement(J);
			return STM4(FOR_VAR, a, b, c, d);
		}
		if (accept(J, TK_IN)) {
			b = expression(J, 0);
			expect(J, ')');
			c = statement(J);
			return STM3(FOR_IN_VAR, a, b, c);
		}
		jsP_error(J, "unexpected token in for-var-statement: %s", TOKSTR);
		return NULL;
	}

	if (J->lookahead != ';')
		a = expression(J, 1);
	else
		a = NULL;
	if (accept(J, ';')) {
		b = forexpression(J, ';');
		c = forexpression(J, ')');
		d = statement(J);
		return STM4(FOR, a, b, c, d);
	}
	if (accept(J, TK_IN)) {
		b = expression(J, 0);
		expect(J, ')');
		c = statement(J);
		return STM3(FOR_IN, a, b, c);
	}
	jsP_error(J, "unexpected token in for-statement: %s", TOKSTR);
	return NULL;
}

static js_Ast *statement(js_State *J)
{
	js_Ast *a, *b, *c, *d;

	if (J->lookahead == '{') {
		return block(J);
	}

	if (accept(J, TK_VAR)) {
		a = vardeclist(J, 0);
		semicolon(J);
		return STM1(VAR, a);
	}

	/* empty statement */
	if (accept(J, ';')) {
		return STM0(NOP);
	}

	if (accept(J, TK_IF)) {
		expect(J, '(');
		a = expression(J, 0);
		expect(J, ')');
		b = statement(J);
		if (accept(J, TK_ELSE))
			c = statement(J);
		else
			c = NULL;
		return STM3(IF, a, b, c);
	}

	if (accept(J, TK_DO)) {
		a = statement(J);
		expect(J, TK_WHILE);
		expect(J, '(');
		b = expression(J, 0);
		expect(J, ')');
		semicolon(J);
		return STM2(DO, a, b);
	}

	if (accept(J, TK_WHILE)) {
		expect(J, '(');
		a = expression(J, 0);
		expect(J, ')');
		b = statement(J);
		return STM2(WHILE, a, b);
	}

	if (accept(J, TK_FOR)) {
		return forstatement(J);
	}

	if (accept(J, TK_CONTINUE)) {
		a = identifieropt(J);
		semicolon(J);
		return STM1(CONTINUE, a);
	}

	if (accept(J, TK_BREAK)) {
		a = identifieropt(J);
		semicolon(J);
		return STM1(BREAK, a);
	}

	if (accept(J, TK_RETURN)) {
		if (J->lookahead != ';' && J->lookahead != '}' && J->lookahead != 0)
			a = expression(J, 0);
		else
			a = NULL;
		semicolon(J);
		return STM1(RETURN, a);
	}

	if (accept(J, TK_WITH)) {
		expect(J, '(');
		a = expression(J, 0);
		expect(J, ')');
		b = statement(J);
		return STM2(WITH, a, b);
	}

	if (accept(J, TK_SWITCH)) {
		expect(J, '(');
		a = expression(J, 0);
		expect(J, ')');
		expect(J, '{');
		b = caselist(J);
		expect(J, '}');
		return STM2(SWITCH, a, b);
	}

	if (accept(J, TK_THROW)) {
		a = expression(J, 0);
		semicolon(J);
		return STM1(THROW, a);
	}

	if (accept(J, TK_TRY)) {
		a = block(J);
		b = c = d = NULL;
		if (accept(J, TK_CATCH)) {
			expect(J, '(');
			b = identifier(J);
			expect(J, ')');
			c = block(J);
		}
		if (accept(J, TK_FINALLY)) {
			d = block(J);
		}
		if (!b && !d)
			jsP_error(J, "unexpected token in try: %s (expected 'catch' or 'finally')", TOKSTR);
		return STM4(TRY, a, b, c, d);
	}

	if (accept(J, TK_DEBUGGER)) {
		semicolon(J);
		return STM0(DEBUGGER);
	}

	if (accept(J, TK_FUNCTION)) {
		jsP_warning(J, "function statements are not standard");
		return funstm(J);
	}

	/* labelled statement or expression statement */
	if (J->lookahead == TK_IDENTIFIER) {
		a = expression(J, 0);
		if (a->type == AST_IDENTIFIER && accept(J, ':')) {
			b = statement(J);
			return STM2(LABEL, a, b);
		}
		semicolon(J);
		return a;
	}

	/* expression statement */
	a = expression(J, 0);
	semicolon(J);
	return a;

	jsP_error(J, "unexpected token in statement: %s", TOKSTR);
	return NULL;
}

/* Program */

static js_Ast *scriptelement(js_State *J)
{
	if (accept(J, TK_FUNCTION))
		return fundec(J);
	return statement(J);
}

static js_Ast *script(js_State *J, int terminator)
{
	js_Ast *head, *tail;
	if (J->lookahead == terminator)
		return NULL;
	head = tail = LIST(scriptelement(J));
	while (J->lookahead != terminator)
		tail = tail->b = LIST(scriptelement(J));
	return jsP_list(head);
}

static js_Ast *funbody(js_State *J)
{
	js_Ast *a;
	expect(J, '{');
	a = script(J, '}');
	expect(J, '}');
	return a;
}

/* Constant folding */

static inline int toint32(double d)
{
	double two32 = 4294967296.0;
	double two31 = 2147483648.0;

	if (!isfinite(d) || d == 0)
		return 0;

	d = fmod(d, two32);
	d = d >= 0 ? floor(d) : ceil(d) + two32;
	if (d >= two31)
		return d - two32;
	else
		return d;
}

static inline unsigned int touint32(double d)
{
	return toint32(d);
}

static int jsP_setnumnode(js_Ast *node, double x)
{
	node->type = AST_NUMBER;
	node->number = x;
	node->a = node->b = node->c = node->d = NULL;
	return 1;
}

static int jsP_foldconst(js_Ast *node)
{
	double x, y;
	int a, b;

	if (node->type == AST_NUMBER)
		return 1;

	a = node->a ? jsP_foldconst(node->a) : 0;
	b = node->b ? jsP_foldconst(node->b) : 0;
	if (node->c) jsP_foldconst(node->c);
	if (node->d) jsP_foldconst(node->d);

	if (a) {
		x = node->a->number;
		switch (node->type) {
		case EXP_NEG: return jsP_setnumnode(node, -x);
		case EXP_POS: return jsP_setnumnode(node, x);
		case EXP_BITNOT: return jsP_setnumnode(node, ~toint32(x));
		}

		if (b) {
			y = node->b->number;
			switch (node->type) {
			case EXP_MUL: return jsP_setnumnode(node, x * y);
			case EXP_DIV: return jsP_setnumnode(node, x / y);
			case EXP_MOD: return jsP_setnumnode(node, fmod(x, y));
			case EXP_ADD: return jsP_setnumnode(node, x + y);
			case EXP_SUB: return jsP_setnumnode(node, x - y);
			case EXP_SHL: return jsP_setnumnode(node, toint32(x) << (touint32(y) & 0x1F));
			case EXP_SHR: return jsP_setnumnode(node, toint32(x) >> (touint32(y) & 0x1F));
			case EXP_USHR: return jsP_setnumnode(node, touint32(x) >> (touint32(y) & 0x1F));
			case EXP_BITAND: return jsP_setnumnode(node, toint32(x) & toint32(y));
			case EXP_BITXOR: return jsP_setnumnode(node, toint32(x) ^ toint32(y));
			case EXP_BITOR: return jsP_setnumnode(node, toint32(x) | toint32(y));
			}
		}
	}

	return 0;
}

/* Main entry point */

js_Ast *jsP_parse(js_State *J, const char *filename, const char *source)
{
	js_Ast *p;

	jsY_initlex(J, filename, source);
	next(J);
	p = script(J, 0);
	if (p)
		jsP_foldconst(p);

	return p;
}
