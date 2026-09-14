#include "pp_expr.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
    Pp *pp;
    const Vec *tokens;
    size_t pos;
    Loc last_loc;
    bool suppress;
} ExprParser;

static PpExprVal expr_ternary(ExprParser *ep);

static PpToken *expr_peek(ExprParser *ep)
{
    return ep->pos < vec_size(ep->tokens) ? vec_get(ep->tokens, ep->pos) : NULL;
}

static bool expr_punct(ExprParser *ep, PpPunct punct)
{
    PpToken *t = expr_peek(ep);
    return t && t->kind == TOK_PP_PUNCT && t->punct == punct;
}

static void expr_advance(ExprParser *ep)
{
    PpToken *t = expr_peek(ep);
    if (t)
    {
        ep->last_loc = t->loc;
    }
    ep->pos++;
}

/* Location of the current token, or of the last one consumed at end. */
static Loc expr_loc(ExprParser *ep)
{
    PpToken *t = expr_peek(ep);
    return t ? t->loc : ep->last_loc;
}

/* Consumes punct when it is next; returns whether it was. */
static bool expr_expect(ExprParser *ep, PpPunct punct)
{
    if (expr_punct(ep, punct))
    {
        expr_advance(ep);
        return true;
    }
    return false;
}

static void expr_error(ExprParser *ep, Loc loc, const char *fmt, ...)
{
    if (ep->suppress)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    pp_verror(ep->pp, loc, fmt, args);
    va_end(args);
}

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool tok_is(const PpToken *t, const char *s)
{
    size_t len = strlen(s);
    return t->len == len && strncmp(t->spell, s, len) == 0;
}

static u8 hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return (u8) (c - '0');
    }
    return (u8) (c - (c >= 'a' ? 'a' : 'A') + 10);
}

static int expr_escape(ExprParser *ep, const char **ppos, const char *end, Loc loc)
{
    static const char simple_codes[] = "abfnrtv\\'\"?";
    static const char simple_vals[] = {'\a', '\b', '\f', '\n', '\r', '\t',
                                       '\v', '\\', '\'', '"',  '?'};

    const char *p = *ppos;
    if (p >= end)
    {
        return 0;
    }
    char c = *p;
    const char *hit = strchr(simple_codes, c);
    if (hit)
    {
        *ppos = p + 1;
        return simple_vals[hit - simple_codes];
    }
    if (c >= '0' && c <= '7')
    {
        int val = 0;
        for (int i = 0; i < 3 && p < end && *p >= '0' && *p <= '7'; i++)
        {
            if (val <= 0xFF)
            {
                val = val * 8 + (*p - '0');
            }
            p++;
        }
        *ppos = p;
        return val;
    }
    if (c == 'x' || c == 'X')
    {
        p++;
        if (p >= end || !is_hex_digit(*p))
        {
            expr_error(ep, loc, "hexadecimal escape sequence with no digits in #if");
            *ppos = p;
            return 0;
        }
        int val = 0;
        while (p < end && is_hex_digit(*p))
        {
            if (val <= 0xFF)
            {
                val = val * 16 + hex_digit_value(*p);
            }
            p++;
        }
        *ppos = p;
        return val;
    }
    *ppos = p + 1;
    return c;
}

static PpExprVal expr_number(ExprParser *ep, const PpToken *tok)
{
    const char *s = tok->spell;
    u32 len = tok->len;
    u32 i = 0;
    u64 base = 10;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        i = 2;
    }
    else if (len >= 2 && s[0] == '0')
    {
        base = 8;
        i = 1;
    }

    u64 val = 0;
    for (; i < len; i++)
    {
        char c = s[i];
        u64 digit;
        if (c >= '0' && c <= '9')
        {
            digit = (u64) (c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = (u64) (c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = (u64) (c - 'A' + 10);
        }
        else
        {
            break;
        }
        if (digit >= base)
        {
            expr_error(ep, tok->loc, "invalid digit in #if integer literal");
            return (PpExprVal) {0, false};
        }
        val = val * base + digit;
    }

    bool suffix_u = false;
    for (; i < len; i++)
    {
        char c = s[i];
        if (c == 'u' || c == 'U')
        {
            suffix_u = true;
        }
        else if (c != 'l' && c != 'L')
        {
            expr_error(ep, tok->loc, "invalid integer literal in #if");
            return (PpExprVal) {0, false};
        }
    }

    bool is_unsigned = suffix_u;
    if (!suffix_u && base != 10 && val >= 0x80000000ULL && val <= 0xFFFFFFFFULL)
    {
        is_unsigned = true;
    }
    return (PpExprVal) {.value = val, .is_unsigned = is_unsigned};
}

static u64 expr_char_value(ExprParser *ep, const PpToken *tok)
{
    const char *p = tok->spell + 1;
    const char *end = tok->spell + tok->len - 1;
    u64 val = 0;
    while (p < end)
    {
        int c;
        if (*p == '\\')
        {
            p++;
            c = expr_escape(ep, &p, end, tok->loc);
        }
        else
        {
            c = (u8) *p;
            p++;
        }
        val = (val << 8) | (u8) c;
    }
    return val;
}

static PpExprVal expr_primary(ExprParser *ep)
{
    PpToken *t = expr_peek(ep);
    if (!t)
    {
        expr_error(ep, ep->last_loc, "unexpected end of #if expression");
        return (PpExprVal) {0, false};
    }

    if (expr_punct(ep, PP_PUNCT_LPAREN))
    {
        expr_advance(ep);
        PpExprVal v = expr_ternary(ep);
        if (!expr_expect(ep, PP_PUNCT_RPAREN))
        {
            expr_error(ep, expr_loc(ep), "expected ')' in #if expression");
            expr_advance(ep);
        }
        return v;
    }

    if (t->kind == TOK_PP_NUMBER)
    {
        expr_advance(ep);
        return expr_number(ep, t);
    }
    if (t->kind == TOK_PP_CHAR)
    {
        expr_advance(ep);
        return (PpExprVal) {.value = expr_char_value(ep, t), .is_unsigned = false};
    }
    if (t->kind == TOK_PP_IDENT)
    {
        if (tok_is(t, "defined") || tok_is(t, "__has_include") || tok_is(t, "__has_include_next"))
        {
            expr_error(ep, t->loc, "operator '%.*s' cannot appear in a macro expansion",
                       (int) t->len, t->spell);
        }
        expr_advance(ep);
        return (PpExprVal) {0, false};
    }

    expr_error(ep, t->loc, "unexpected token in #if expression");
    expr_advance(ep);
    return (PpExprVal) {0, false};
}

static PpExprVal expr_unary(ExprParser *ep)
{
    PpToken *t = expr_peek(ep);
    if (t && t->kind == TOK_PP_PUNCT)
    {
        PpPunct op = t->punct;
        if (op == PP_PUNCT_PLUS || op == PP_PUNCT_MINUS || op == PP_PUNCT_BANG ||
            op == PP_PUNCT_TILDE)
        {
            expr_advance(ep);
            PpExprVal v = expr_unary(ep);
            switch (op)
            {
                case PP_PUNCT_PLUS:
                    return v;
                case PP_PUNCT_MINUS:
                    return (PpExprVal) {0ULL - v.value, v.is_unsigned};
                case PP_PUNCT_BANG:
                    return (PpExprVal) {v.value == 0 ? 1 : 0, false};
                case PP_PUNCT_TILDE:
                    return (PpExprVal) {~v.value, v.is_unsigned};
                default:
                    break;
            }
        }
    }
    return expr_primary(ep);
}

static bool expr_div_guard(ExprParser *ep, Loc loc, u64 a, u64 b, bool u, const char *what)
{
    if (b == 0)
    {
        expr_error(ep, loc, "division by zero in #if");
        return true;
    }
    if (!u && (i64) a == INT64_MIN && (i64) b == -1)
    {
        expr_error(ep, loc, "overflow in %s in #if", what);
        return true;
    }
    return false;
}

static PpExprVal expr_binop(ExprParser *ep, PpPunct op, PpExprVal l, PpExprVal r, Loc loc)
{
    bool u = l.is_unsigned || r.is_unsigned;
    u64 a = l.value;
    u64 b = r.value;
    i64 sa = (i64) a;
    i64 sb = (i64) b;

    switch (op)
    {
        case PP_PUNCT_PLUS:
            return (PpExprVal) {a + b, u};
        case PP_PUNCT_MINUS:
            return (PpExprVal) {a - b, u};
        case PP_PUNCT_STAR:
            return (PpExprVal) {a * b, u};
        case PP_PUNCT_SLASH:
            if (expr_div_guard(ep, loc, a, b, u, "division"))
            {
                return (PpExprVal) {0, u};
            }
            return u ? (PpExprVal) {a / b, true} : (PpExprVal) {(u64) (sa / sb), false};
        case PP_PUNCT_PERCENT:
            if (expr_div_guard(ep, loc, a, b, u, "remainder"))
            {
                return (PpExprVal) {0, u};
            }
            return u ? (PpExprVal) {a % b, true} : (PpExprVal) {(u64) (sa % sb), false};
        case PP_PUNCT_SHL:
        case PP_PUNCT_SHR:
            if (b >= 64)
            {
                expr_error(ep, loc, "shift count exceeds type width in #if");
                return (PpExprVal) {0, u};
            }
            if (op == PP_PUNCT_SHL)
            {
                return (PpExprVal) {a << b, u};
            }
            return u ? (PpExprVal) {a >> b, true} : (PpExprVal) {(u64) (sa >> b), false};
        case PP_PUNCT_LT:
            if (u)
            {
                return (PpExprVal) {a < b, false};
            }
            return (PpExprVal) {sa < sb, false};
        case PP_PUNCT_GT:
            if (u)
            {
                return (PpExprVal) {a > b, false};
            }
            return (PpExprVal) {sa > sb, false};
        case PP_PUNCT_LE:
            if (u)
            {
                return (PpExprVal) {a <= b, false};
            }
            return (PpExprVal) {sa <= sb, false};
        case PP_PUNCT_GE:
            if (u)
            {
                return (PpExprVal) {a >= b, false};
            }
            return (PpExprVal) {sa >= sb, false};
        case PP_PUNCT_EQ:
            return (PpExprVal) {a == b, false};
        case PP_PUNCT_NE:
            return (PpExprVal) {a != b, false};
        case PP_PUNCT_AMP:
            return (PpExprVal) {a & b, u};
        case PP_PUNCT_CARET:
            return (PpExprVal) {a ^ b, u};
        case PP_PUNCT_PIPE:
            return (PpExprVal) {a | b, u};
        default:
            expr_error(ep, loc, "internal error: unexpected #if operator");
            return (PpExprVal) {0, u};
    }
}

typedef PpExprVal (*NextLevel)(ExprParser *);

/* Parses an operand that C never evaluates (short-circuited &&/||/?:) with
   diagnostics suppressed. */
static void expr_skip(ExprParser *ep, NextLevel next)
{
    bool saved = ep->suppress;
    ep->suppress = true;
    next(ep);
    ep->suppress = saved;
}

static PpExprVal expr_bin_level(ExprParser *ep, NextLevel next, PpPunct op1, PpPunct op2,
                                PpPunct op3, PpPunct op4)
{
    PpExprVal l = next(ep);
    for (;;)
    {
        PpPunct op = PP_PUNCT_NONE;
        if (expr_punct(ep, op1))
        {
            op = op1;
        }
        else if (op2 != PP_PUNCT_NONE && expr_punct(ep, op2))
        {
            op = op2;
        }
        else if (op3 != PP_PUNCT_NONE && expr_punct(ep, op3))
        {
            op = op3;
        }
        else if (op4 != PP_PUNCT_NONE && expr_punct(ep, op4))
        {
            op = op4;
        }
        else
        {
            break;
        }
        Loc loc = expr_loc(ep);
        expr_advance(ep);
        l = expr_binop(ep, op, l, next(ep), loc);
    }
    return l;
}

static PpExprVal expr_mul(ExprParser *ep)
{
    return expr_bin_level(ep, expr_unary, PP_PUNCT_STAR, PP_PUNCT_SLASH, PP_PUNCT_PERCENT,
                          PP_PUNCT_NONE);
}

static PpExprVal expr_add(ExprParser *ep)
{
    return expr_bin_level(ep, expr_mul, PP_PUNCT_PLUS, PP_PUNCT_MINUS, PP_PUNCT_NONE,
                          PP_PUNCT_NONE);
}

static PpExprVal expr_shift(ExprParser *ep)
{
    return expr_bin_level(ep, expr_add, PP_PUNCT_SHL, PP_PUNCT_SHR, PP_PUNCT_NONE, PP_PUNCT_NONE);
}

static PpExprVal expr_rel(ExprParser *ep)
{
    return expr_bin_level(ep, expr_shift, PP_PUNCT_LT, PP_PUNCT_GT, PP_PUNCT_LE, PP_PUNCT_GE);
}

static PpExprVal expr_eq(ExprParser *ep)
{
    return expr_bin_level(ep, expr_rel, PP_PUNCT_EQ, PP_PUNCT_NE, PP_PUNCT_NONE, PP_PUNCT_NONE);
}

static PpExprVal expr_band(ExprParser *ep)
{
    return expr_bin_level(ep, expr_eq, PP_PUNCT_AMP, PP_PUNCT_NONE, PP_PUNCT_NONE, PP_PUNCT_NONE);
}

static PpExprVal expr_bxor(ExprParser *ep)
{
    return expr_bin_level(ep, expr_band, PP_PUNCT_CARET, PP_PUNCT_NONE, PP_PUNCT_NONE,
                          PP_PUNCT_NONE);
}

static PpExprVal expr_bor(ExprParser *ep)
{
    return expr_bin_level(ep, expr_bxor, PP_PUNCT_PIPE, PP_PUNCT_NONE, PP_PUNCT_NONE,
                          PP_PUNCT_NONE);
}

static PpExprVal expr_land(ExprParser *ep)
{
    PpExprVal l = expr_bor(ep);
    while (expr_expect(ep, PP_PUNCT_ANDAND))
    {
        if (l.value != 0)
        {
            PpExprVal r = expr_bor(ep);
            l.value = r.value != 0 ? 1 : 0;
        }
        else
        {
            expr_skip(ep, expr_bor);
        }
    }
    l.is_unsigned = false;
    return l;
}

static PpExprVal expr_lor(ExprParser *ep)
{
    PpExprVal l = expr_land(ep);
    while (expr_expect(ep, PP_PUNCT_OROR))
    {
        if (l.value != 0)
        {
            expr_skip(ep, expr_land);
        }
        else
        {
            PpExprVal r = expr_land(ep);
            l.value = r.value != 0 ? 1 : 0;
        }
    }
    l.is_unsigned = false;
    return l;
}

static PpExprVal expr_ternary(ExprParser *ep)
{
    PpExprVal cond = expr_lor(ep);
    if (!expr_expect(ep, PP_PUNCT_QUESTION))
    {
        return cond;
    }

    PpExprVal yes;
    if (cond.value != 0)
    {
        yes = expr_ternary(ep);
    }
    else
    {
        yes = (PpExprVal) {0, false};
        expr_skip(ep, expr_ternary);
    }

    if (!expr_expect(ep, PP_PUNCT_COLON))
    {
        expr_error(ep, expr_loc(ep), "expected ':' in #if expression");
        expr_advance(ep);
    }

    PpExprVal no;
    if (cond.value != 0)
    {
        no = (PpExprVal) {0, false};
        expr_skip(ep, expr_ternary);
    }
    else
    {
        no = expr_ternary(ep);
    }
    return cond.value != 0 ? yes : no;
}

PpExprVal pp_eval_expr(Pp *pp, const Vec *tokens)
{
    ExprParser ep = {.pp = pp, .tokens = tokens, .pos = 0};
    PpExprVal v = expr_ternary(&ep);
    if (expr_peek(&ep))
    {
        expr_error(&ep, expr_loc(&ep), "unexpected token after #if expression");
    }
    return v;
}
