// Mert Aga 241ADB159
// https://github.com/mertaga146-lab/Mert-Aga-C
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI) for program structure and code review; conversation history: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167. Details in comments where used.

// With -std=c17 glibc hides POSIX functions (mkdir) unless this is defined.
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STUDENT_NAME "mert"
#define STUDENT_LAST "aga"
#define STUDENT_ID "241adb159"

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */
/* ------------------------------------------------------------------ */

// Results are collected in memory and written once at the end. We must do
// this because a single error anywhere in the file replaces ALL results with
// just "ERROR:<pos>", so we cannot write partial output as we go.
// Capacity doubles on growth, so appending n bytes costs O(n) in total even
// for multi-megabyte inputs (no repeated copying of the whole buffer).
// AI (Claude): I pasted the lab text and asked (in Turkish) to do the task
// stage by stage, starting with a summary of the plan. Claude proposed
// collecting results in a growable buffer and wrote Buf/buf_append (stage 1).
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + extra) ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd) return 0;
    b->data = nd;
    b->cap = ncap;
    return 1;
}

static int buf_append(Buf *b, const char *s, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

// AI (Claude): after my repo and Codespace were ready and I uploaded
// calc-tests.zip, Claude wrote this tokenizer (stage 1): tokens that remember
// their offset in the whole file, plus the T_EOL token used for error positions.
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,
    T_BAD
} TokKind;

// AI (Claude): stage 7. Claude wrote the float support (Value, the float
// literal in the tokenizer, float_result and the float branches of the parser).
// I replaced my file with it and ran run_tests.sh and cli.sh.
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
// Integer or float? A number is a float only if its literal contains a '.';
// the result of an operation is a float only if at least one operand is. So a
// line without floats never touches double: it keeps exact long long
// arithmetic and C's truncating '/'. As soon as one operand is a float the
// whole operation is done in double, as the task requires (7 / 2.0 is 3.5).
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

static Value make_int(long long i) {
    Value v = {0, i, 0.0};
    return v;
}

static Value make_float(double d) {
    Value v = {1, 0, d};
    return v;
}

static double as_double(Value v) {
    return v.is_float ? v.d : (double)v.i;
}

typedef struct {
    TokKind kind;
    size_t pos;     // 0-based offset in the WHOLE file (not in the line)
    Value val;      // the number, when kind == T_NUM
} Token;

// One Parser is reused for every line. src is the whole file; a line is just
// the range [cur, end) inside it.
typedef struct {
    char *src;      // not const: next_token briefly writes a NUL (see strtod)
    size_t end;     // offset of this line's '\n' (or file length)
    size_t cur;
    Token tok;      // lookahead: the token the parser is "looking at"
    int failed;
    int oom;        // set when malloc failed; the line is then not an ERROR result
    size_t err_pos;
} Parser;

// Positions: instead of counting characters line by line, every token stores
// its offset into the single in-memory copy of the file. The file offset
// already counts every character including each '\n' and comment lines, which
// is exactly how the task defines positions. We add 1 only when printing.
static void next_token(Parser *p) {
    while (p->cur < p->end && (p->src[p->cur] == ' ' || p->src[p->cur] == '\t'))
        p->cur++;

    Token t;
    t.pos = p->cur;
    t.val = make_int(0);

    if (p->cur >= p->end) {
        // EOL is reported at the '\n' itself, or at file length + 1 (in
        // 1-based terms) when the last line has no '\n'. Both are simply
        // "offset == end", so no special case is needed.
        t.kind = T_EOL;
        p->tok = t;
        return;
    }

    char c = p->src[p->cur];
    if (isdigit((unsigned char)c)) {
        size_t start = p->cur;
        // unsigned: a huge digit string wraps around instead of being
        // undefined behaviour (the tests never overflow anyway).
        unsigned long long v = 0;
        while (p->cur < p->end && isdigit((unsigned char)p->src[p->cur])) {
            v = v * 10 + (unsigned long long)(p->src[p->cur] - '0');
            p->cur++;
        }
        // A float needs digits on BOTH sides of the '.': ".5" and "5." are
        // errors. So the '.' is only consumed when a digit follows it.
        // Otherwise it stays unread and the next call turns it into T_BAD,
        // which puts the error exactly on the '.' ("2. + 5" fails at position
        // 2). The same trick handles 'e': "1.5e3" stops after 1.5 and the 'e'
        // becomes the error token.
        if (p->cur + 1 < p->end && p->src[p->cur] == '.' &&
            isdigit((unsigned char)p->src[p->cur + 1])) {
            p->cur++;
            while (p->cur < p->end && isdigit((unsigned char)p->src[p->cur]))
                p->cur++;
            // strtod rounds correctly (0.1 + 0.2 must print 0.3) but needs a
            // NUL-terminated string. Copying every literal would cost an
            // allocation per number on multi-megabyte files, so we put a NUL
            // right after the literal for the call and restore the byte.
            char saved = p->src[p->cur];
            p->src[p->cur] = '\0';
            double d = strtod(p->src + start, NULL);
            p->src[p->cur] = saved;
            t.val = make_float(d);
        } else {
            t.val = make_int((long long)v);
        }
        t.kind = T_NUM;
        p->tok = t;
        return;
    }

    p->cur++;
    switch (c) {
    case '+': t.kind = T_PLUS; break;
    case '-': t.kind = T_MINUS; break;
    case '/': t.kind = T_SLASH; break;
    case '(': t.kind = T_LPAREN; break;
    case ')': t.kind = T_RPAREN; break;
    case '*':
        // Greedy: "**" is one token, otherwise "2 ** 3" would be read as two
        // '*' operators and be reported as "two operators in a row".
        if (p->cur < p->end && p->src[p->cur] == '*') {
            p->cur++;
            t.kind = T_POW;
        } else {
            t.kind = T_STAR;
        }
        break;
    default: t.kind = T_BAD; break;
    }
    p->tok = t;
}

/* ------------------------------------------------------------------ */
/* Parser (recursive descent)                                          */
/* ------------------------------------------------------------------ */

// Only the first error matters, so later calls must not overwrite it.
// No setjmp (not in the allowed headers), so every function checks p->failed.
static void fail(Parser *p, size_t pos) {
    if (!p->failed) {
        p->failed = 1;
        p->err_pos = pos;
    }
}

// A float operation can give inf (10.0 ** 400) or nan ((0 - 8) ** 0.5). Both
// are errors reported at the OPERATOR: it is the token that produced the bad
// value, while the operands on both sides were fine.
static Value float_result(Parser *p, double r, size_t op_pos) {
    if (!isfinite(r)) {
        fail(p, op_pos);
        return make_int(0);
    }
    return make_float(r);
}

// AI (Claude): stage 4. I wrote "baslayak" (let's start) and Claude wrote the
// parenthesis handling in parse_primary (the missing/extra ")" rules). I ran
// run_tests.sh afterwards: 28 passed at that point.
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
// parse_primary and parse_expr call each other (a parenthesis starts a whole
// new expression), so one of them needs a forward declaration.
static Value parse_expr(Parser *p);

// primary := NUMBER | '(' expr ')'
// A '(' restarts the grammar at the lowest level (expr), which is why
// parentheses override precedence: in (2 + 3) * 4 the + is fully evaluated
// inside primary before term ever sees the '*'.
// Every other token is an error at that token's start: the parser "could not
// continue" there. This one rule covers an invalid character, a second
// operator in a row ("7 + * 3" fails at '*'), a missing operand at end of line
// (the EOL token's position) and an empty "()" (fails at the ')').
static Value parse_primary(Parser *p) {
    if (p->tok.kind == T_NUM) {
        Value v = p->tok.val;
        next_token(p);
        return v;
    }
    if (p->tok.kind == T_LPAREN) {
        next_token(p);
        Value v = parse_expr(p);
        if (p->failed) return make_int(0);
        // Missing ')' is reported at whatever token we are looking at instead
        // of ')': at end of line that is the EOL token, i.e. the line's '\n'
        // (or file length + 1), as required. An EXTRA ')' is not handled
        // here; it is left over and eval_line reports it.
        if (p->tok.kind != T_RPAREN) {
            fail(p, p->tok.pos);
            return make_int(0);
        }
        next_token(p);
        return v;
    }
    fail(p, p->tok.pos);
    return make_int(0);
}

// AI (Claude): stage 5. Claude wrote parse_power (** right-associative,
// negative exponent error, square-and-multiply) and changed parse_term to
// call it; I replaced the code in my file and ran the tests (33 passed).
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
// power := primary [ '**' power ]
// The grammar is right-recursive: the exponent is itself a power, so
// 2 ** 3 ** 2 groups as 2 ** (3 ** 2) = 512. That is why ** is
// right-associative (a left-to-right loop like in expr/term would give
// (2 ** 3) ** 2). power sits BELOW term, so 2 * 3 ** 2 is 2 * (3 ** 2): the
// exponent is already computed when term sees the '*'.
// The rule is run as a loop, not as real recursion: all operands of a chain
// a ** b ** c ** ... are collected first and then folded from the RIGHT end,
// which gives exactly the same grouping. One recursive call per ** would
// overflow the C stack on a line with a few hundred thousand of them.
typedef struct {
    Value v;
    size_t op_pos;    // position of the '**' in front of this operand
    size_t start_pos; // where this operand starts: a bad exponent is reported there
} PowItem;

// base ** exp for two already evaluated operands.
static Value power_op(Parser *p, Value base, Value exp, size_t op_pos,
                      size_t exp_pos) {
    if (base.is_float || exp.is_float) {
        // Float arithmetic: a negative exponent is fine here (0.5 ** (0 - 1)
        // is 2). Only inf/nan are errors, reported at the operator.
        return float_result(p, pow(as_double(base), as_double(exp)), op_pos);
    }

    // int ** int: a negative exponent would not give an integer. It is
    // reported at the START of the exponent.
    if (exp.i < 0) {
        fail(p, exp_pos);
        return make_int(0);
    }

    // Square-and-multiply: O(log exp) multiplications, so something like
    // 1 ** 1000000000 is instant. b is only squared while bits remain,
    // which avoids a useless final squaring that could overflow.
    // x ** 0 stays 1 for every x, including 0 ** 0.
    long long result = 1;
    long long b = base.i;
    long long e = exp.i;
    while (e > 0) {
        if (e & 1) result *= b;
        e >>= 1;
        if (e > 0) b *= b;
    }
    return make_int(result);
}

static Value parse_power(Parser *p) {
    Value first = parse_primary(p);
    if (p->failed || p->tok.kind != T_POW) return first;

    // Most chains are short, so start on the stack and only use the heap for
    // long ones.
    PowItem small[8];
    PowItem *items = small;
    size_t cap = 8;
    size_t n = 0;
    items[n].v = first;
    items[n].op_pos = 0;
    items[n].start_pos = 0;
    n++;

    while (p->tok.kind == T_POW) {
        size_t op_pos = p->tok.pos;
        next_token(p);
        size_t start_pos = p->tok.pos;
        Value v = parse_primary(p);
        if (p->failed) break;
        if (n == cap) {
            PowItem *bigger = realloc(items == small ? NULL : items,
                                      cap * 2 * sizeof *bigger);
            if (!bigger) {
                p->oom = 1;
                fail(p, p->tok.pos);
                break;
            }
            if (items == small) memcpy(bigger, small, n * sizeof *bigger);
            items = bigger;
            cap *= 2;
        }
        items[n].v = v;
        items[n].op_pos = op_pos;
        items[n].start_pos = start_pos;
        n++;
    }

    Value acc = make_int(0);
    if (!p->failed) {
        acc = items[n - 1].v;
        for (size_t k = n - 1; k >= 1 && !p->failed; k--)
            acc = power_op(p, items[k - 1].v, acc, items[k].op_pos,
                           items[k].start_pos);
    }
    if (items != small) free(items);
    return p->failed ? make_int(0) : acc;
}

// AI (Claude): stage 3. Claude wrote parse_term (* and /, precedence,
// division-by-zero position) and rewired parse_expr to call it. I applied it
// and ran run_tests.sh (20 passed at that point).
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
// term := power { ('*' | '/') power }
// term sits BELOW expr in the grammar, so expr only ever sees values that term
// has already multiplied/divided. In "3 + 2 * 5", after '+' expr asks term for
// its right side and term consumes "2 * 5" first. That is what makes * and /
// bind tighter than + and -. The loop makes them left-associative:
// 100 / 7 / 2 is (100 / 7) / 2 = 7.
static Value parse_term(Parser *p) {
    Value left = parse_power(p);
    while (!p->failed && (p->tok.kind == T_STAR || p->tok.kind == T_SLASH)) {
        TokKind op = p->tok.kind;
        size_t op_pos = p->tok.pos;
        next_token(p);
        // Remember where the right operand starts BEFORE parsing it: a zero
        // divisor is reported at that operand, not at the '/'.
        size_t right_pos = p->tok.pos;
        Value right = parse_power(p);
        if (p->failed) break;

        if (!left.is_float && !right.is_float) {
            if (op == T_STAR) {
                left = make_int(left.i * right.i);
            } else {
                if (right.i == 0) {
                    fail(p, right_pos);
                    break;
                }
                // C's integer '/' already truncates toward zero, as required.
                left = make_int(left.i / right.i);
            }
        } else {
            double a = as_double(left);
            double b = as_double(right);
            if (op == T_STAR) {
                left = float_result(p, a * b, op_pos);
            } else {
                // Float division by zero is the same error as the integer one.
                if (b == 0.0) {
                    fail(p, right_pos);
                    break;
                }
                left = float_result(p, a / b, op_pos);
            }
            if (p->failed) break;
        }
    }
    return left;
}

// expr := term { ('+' | '-') term }
// The loop (not recursion) makes + and - left-associative: 10 - 4 + 1 is
// (10 - 4) + 1 = 7, not 10 - (4 + 1).
static Value parse_expr(Parser *p) {
    Value left = parse_term(p);
    while (!p->failed && (p->tok.kind == T_PLUS || p->tok.kind == T_MINUS)) {
        TokKind op = p->tok.kind;
        size_t op_pos = p->tok.pos;
        next_token(p);
        Value right = parse_term(p);
        if (p->failed) break;

        if (!left.is_float && !right.is_float) {
            left = make_int(op == T_PLUS ? left.i + right.i : left.i - right.i);
        } else {
            double a = as_double(left);
            double b = as_double(right);
            left = float_result(p, op == T_PLUS ? a + b : a - b, op_pos);
            if (p->failed) break;
        }
    }
    return left;
}

// Evaluates the line src[start, end). Returns 1 on success and stores the
// value, 0 on error and stores the 0-based error offset in *err_pos, -1 if
// memory ran out.
static int eval_line(char *src, size_t start, size_t end,
                     Value *out, size_t *err_pos) {
    Parser p;
    p.src = src;
    p.cur = start;
    p.end = end;
    p.failed = 0;
    p.oom = 0;
    p.err_pos = 0;
    next_token(&p);

    Value v = parse_expr(&p);
    // Leftover tokens ("2 3", or an unexpected ')' later) are an error at
    // the first token the expression did not consume.
    if (!p.failed && p.tok.kind != T_EOL) fail(&p, p.tok.pos);

    if (p.oom) return -1;
    if (p.failed) {
        *err_pos = p.err_pos;
        return 0;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

// AI (Claude): read_file, input_base, ensure_dir and process_file were written
// by Claude in stage 1 (output file naming, no partial results on error).
// Stage 5 added the '#' comment-line check inside process_file: I asked Claude
// for the code and it gave me the changed file, which I replaced and tested.
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167
// Reads the whole file once. Everything else works on offsets into this one
// buffer, so the lines are never copied or split into separate strings.
static char *read_file(const char *path, size_t *len_out) {
    // fopen("rb") happily opens a folder on Linux and ftell then returns a
    // nonsense size, so reject folders up front with a readable error.
    struct stat st;
    errno = 0;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[n] = '\0';
    *len_out = n;
    return data;
}

static char *input_base(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".txt") == 0) n -= 4;
    char *base = malloc(n + 1);
    if (!base) return NULL;
    memcpy(base, name, n);
    base[n] = '\0';
    return base;
}

static int ensure_dir(const char *out_dir) {
    if (mkdir(out_dir, 0777) == 0) return 1;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(out_dir, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    }
    return 0;
}

// Evaluates one input file and writes <out_dir>/<base>_<name>_<last>_<id>.txt.
// Returns 0 on success (including an ERROR:<pos> result), 1 on I/O failure.
static int process_file(const char *in_path, const char *out_dir) {
    size_t len = 0;
    char *src = read_file(in_path, &len);
    if (!src) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path,
                errno ? strerror(errno) : "out of memory");
        return 1;
    }

    // Created only now, after the input was read successfully, so a missing
    // input file does not leave an empty output folder behind.
    if (!ensure_dir(out_dir)) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n", out_dir,
                strerror(errno));
        free(src);
        return 1;
    }

    Buf out = {NULL, 0, 0};
    int failed = 0;
    size_t err_pos = 0;
    int oom = 0;

    // Walk the file line by line with memchr: each byte is looked at a
    // constant number of times, so cost is linear in file size.
    size_t ls = 0;
    while (ls < len && !failed && !oom) {
        const char *nl = memchr(src + ls, '\n', len - ls);
        size_t le = nl ? (size_t)(nl - src) : len;

        size_t i = ls;
        while (i < le && (src[i] == ' ' || src[i] == '\t')) i++;
        // Blank lines (i == le) and comment lines produce no output and are
        // never errors. A line is a comment only if its FIRST non-blank
        // character is '#'; a '#' later in the line is left to the tokenizer,
        // which rejects it as an invalid character. We only skip the line's
        // evaluation, not its bytes: ls still advances by the real line
        // length, so error positions keep counting every comment character.
        if (i < le && src[i] != '#') {
            Value v;
            int r = eval_line(src, ls, le, &v, &err_pos);
            if (r < 0) {
                oom = 1;
            } else if (r > 0) {
                // %.15g: 15 significant digits, so 0.1 + 0.2 prints 0.3 and
                // 1.25 * 4 prints 5, as the task specifies for floats.
                char tmp[48];
                int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                                   : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
                if (!buf_append(&out, tmp, (size_t)n)) oom = 1;
            } else {
                failed = 1;
            }
        }
        ls = le + 1;
    }

    int rc = 0;
    if (oom) {
        fprintf(stderr, "calc: out of memory\n");
        rc = 1;
    } else {
        if (failed) {
            // Discard everything gathered so far: no partial results.
            out.len = 0;
            char tmp[48];
            int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", err_pos + 1);
            if (!buf_append(&out, tmp, (size_t)n)) {
                fprintf(stderr, "calc: out of memory\n");
                rc = 1;
            }
        }
        if (rc == 0) {
            char *base = input_base(in_path);
            size_t plen = strlen(out_dir) + strlen(base ? base : "") + 64;
            char *path = base ? malloc(plen) : NULL;
            if (!path) {
                fprintf(stderr, "calc: out of memory\n");
                rc = 1;
            } else {
                snprintf(path, plen, "%s/%s_%s_%s_%s.txt", out_dir, base,
                         STUDENT_NAME, STUDENT_LAST, STUDENT_ID);
                FILE *f = fopen(path, "wb");
                if (!f || (out.len && fwrite(out.data, 1, out.len, f) != out.len)) {
                    fprintf(stderr, "calc: cannot write '%s': %s\n", path,
                            strerror(errno));
                    rc = 1;
                }
                if (f && fclose(f) != 0) rc = 1;
            }
            free(path);
            free(base);
        }
    }

    free(out.data);
    free(src);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

// AI (Claude): stage 6. Claude proposed it as the next step of our
// stage-by-stage plan and wrote process_dir, dir_base, default_out_dir and the
// new main (-d/--dir, default output folder, exit codes). I replaced my file
// and ran run_tests.sh and cli.sh (cli: all passed).
// Link: https://claude.ai/chat/39e8c995-be44-4e1d-8415-1afe7ff12167

// Last component of a folder path, ignoring trailing slashes:
// "../in/" -> "in". With -d it names the default output folder.
static char *dir_base(const char *path) {
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = end - start;
    char *base = malloc(n + 1);
    if (!base) return NULL;
    memcpy(base, path + start, n);
    base[n] = '\0';
    return base;
}

// Default output folder "<base>_<name>_<studentid>" in the current folder.
static char *default_out_dir(const char *base) {
    size_t n = strlen(base) + strlen(STUDENT_NAME) + strlen(STUDENT_ID) + 3;
    char *d = malloc(n);
    if (!d) return NULL;
    snprintf(d, n, "%s_%s_%s", base, STUDENT_NAME, STUDENT_ID);
    return d;
}

// Evaluates every regular *.txt file directly inside dir (no recursion).
// One bad file must not stop the others, so failures are remembered in rc
// and reported through the exit code at the end.
static int process_dir(const char *dir, const char *out_dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir,
                strerror(errno));
        return 1;
    }
    if (!ensure_dir(out_dir)) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n", out_dir,
                strerror(errno));
        closedir(d);
        return 1;
    }

    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n <= 4 || strcmp(e->d_name + n - 4, ".txt") != 0) continue;

        size_t plen = strlen(dir) + n + 2;
        char *path = malloc(plen);
        if (!path) {
            fprintf(stderr, "calc: out of memory\n");
            rc = 1;
            break;
        }
        snprintf(path, plen, "%s/%s", dir, e->d_name);

        // A subfolder could be called "x.txt"; only regular files count.
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (process_file(path, out_dir) != 0) rc = 1;
        }
        free(path);
    }
    closedir(d);
    return rc;
}

static void usage(void) {
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

// Exit codes: 0 = all files processed (an ERROR:<pos> result is a normal
// result, not a failure), 1 = something could not be read or written,
// 2 = wrong command line.
int main(int argc, char **argv) {
    const char *out_dir = NULL;
    const char *in_dir = NULL;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            in_dir = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        } else if (input) {
            fprintf(stderr, "calc: more than one input file given\n");
            usage();
            return 2;
        } else {
            input = argv[i];
        }
    }

    // Exactly one of: an input file, or -d DIR.
    if ((!input && !in_dir) || (input && in_dir)) { usage(); return 2; }

    char *auto_out = NULL;
    if (!out_dir) {
        char *base = in_dir ? dir_base(in_dir) : input_base(input);
        auto_out = base ? default_out_dir(base) : NULL;
        free(base);
        if (!auto_out) {
            fprintf(stderr, "calc: out of memory\n");
            return 1;
        }
        out_dir = auto_out;
    }

    int rc = in_dir ? process_dir(in_dir, out_dir) : process_file(input, out_dir);
    free(auto_out);
    return rc;
}