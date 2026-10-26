// Mert Aga 241ADB159
// https://github.com/mertaga146-lab/Mert-Aga-C
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI) for program structure and code review; conversation history: <PASTE CHAT SHARE LINK HERE>. Details in comments where used.

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

// Used to build "<input>_<name>_<lastname>_<studentid>.txt".
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
// AI (Claude): prompt "how do I read a whole file into memory in C without
// knowing its size" / "growable buffer with doubling in C" Link: <PASTE LINK>
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

typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,  // end of the current line (the '\n', or end of file)
    T_BAD   // a character that cannot start any token
} TokKind;

typedef struct {
    TokKind kind;
    size_t pos;     // 0-based offset in the WHOLE file (not in the line)
    long long ival; // value when kind == T_NUM
} Token;

// One Parser is reused for every line. src is the whole file; a line is just
// the range [cur, end) inside it.
typedef struct {
    const char *src;
    size_t end;     // offset of this line's '\n' (or file length)
    size_t cur;     // next unread offset
    Token tok;      // lookahead: the token the parser is "looking at"
    int failed;
    size_t err_pos; // 0-based offset of the first error on this line
} Parser;

// Positions: instead of counting characters line by line, every token stores
// its offset into the single in-memory copy of the file. The file offset
// already counts every character including each '\n' and comment lines, which
// is exactly how the task defines positions. We add 1 only when printing.
static void next_token(Parser *p) {
    // Spaces and tabs are allowed anywhere between tokens.
    while (p->cur < p->end && (p->src[p->cur] == ' ' || p->src[p->cur] == '\t'))
        p->cur++;

    Token t;
    t.pos = p->cur;
    t.ival = 0;

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
        long long v = 0;
        while (p->cur < p->end && isdigit((unsigned char)p->src[p->cur])) {
            v = v * 10 + (p->src[p->cur] - '0');
            p->cur++;
        }
        t.kind = T_NUM;
        t.ival = v;
        // A '.' right after the digits is left unread on purpose: for now it
        // becomes T_BAD on the next call, so "2. + 5" fails at the '.'
        // (position 2) as the tests expect. Floats come in a later stage.
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

// parse_primary and parse_expr call each other (a parenthesis starts a whole
// new expression), so one of them needs a forward declaration.
static long long parse_expr(Parser *p);

// primary := NUMBER | '(' expr ')'
// A '(' restarts the grammar at the lowest level (expr), which is why
// parentheses override precedence: in (2 + 3) * 4 the + is fully evaluated
// inside primary before term ever sees the '*'.
// Every other token is an error at that token's start: the parser "could not
// continue" there. This one rule covers an invalid character, a second
// operator in a row ("7 + * 3" fails at '*'), a missing operand at end of line
// (the EOL token's position) and an empty "()" (fails at the ')').
static long long parse_primary(Parser *p) {
    if (p->tok.kind == T_NUM) {
        long long v = p->tok.ival;
        next_token(p);
        return v;
    }
    if (p->tok.kind == T_LPAREN) {
        next_token(p);
        long long v = parse_expr(p);
        if (p->failed) return 0;
        // Missing ')' is reported at whatever token we are looking at instead
        // of ')': at end of line that is the EOL token, i.e. the line's '\n'
        // (or file length + 1), as required. An EXTRA ')' is not handled
        // here; it is left over and eval_line reports it.
        if (p->tok.kind != T_RPAREN) {
            fail(p, p->tok.pos);
            return 0;
        }
        next_token(p);
        return v;
    }
    fail(p, p->tok.pos);
    return 0;
}

// term := primary { ('*' | '/') primary }
// term sits BELOW expr in the grammar, so expr only ever sees values that term
// has already multiplied/divided. In "3 + 2 * 5", after '+' expr asks term for
// its right side and term consumes "2 * 5" first. That is what makes * and /
// bind tighter than + and -. The loop makes them left-associative:
// 100 / 7 / 2 is (100 / 7) / 2 = 7.
static long long parse_term(Parser *p) {
    long long left = parse_primary(p);
    while (!p->failed && (p->tok.kind == T_STAR || p->tok.kind == T_SLASH)) {
        TokKind op = p->tok.kind;
        next_token(p);
        // Remember where the right operand starts BEFORE parsing it: a zero
        // divisor is reported at that operand, not at the '/'.
        size_t right_pos = p->tok.pos;
        long long right = parse_primary(p);
        if (p->failed) break;
        if (op == T_STAR) {
            left = left * right;
        } else {
            if (right == 0) {
                fail(p, right_pos);
                break;
            }
            // C's integer '/' already truncates toward zero, as required.
            left = left / right;
        }
    }
    return left;
}

// expr := term { ('+' | '-') term }
// The loop (not recursion) makes + and - left-associative: 10 - 4 + 1 is
// (10 - 4) + 1 = 7, not 10 - (4 + 1).
static long long parse_expr(Parser *p) {
    long long left = parse_term(p);
    while (!p->failed && (p->tok.kind == T_PLUS || p->tok.kind == T_MINUS)) {
        TokKind op = p->tok.kind;
        next_token(p);
        long long right = parse_term(p);
        if (p->failed) break;
        left = (op == T_PLUS) ? left + right : left - right;
    }
    return left;
}

// Evaluates the line src[start, end). Returns 1 on success and stores the
// value, 0 on error and stores the 0-based error offset in *err_pos.
static int eval_line(const char *src, size_t start, size_t end,
                     long long *out, size_t *err_pos) {
    Parser p;
    p.src = src;
    p.cur = start;
    p.end = end;
    p.failed = 0;
    p.err_pos = 0;
    next_token(&p);

    long long v = parse_expr(&p);
    // Leftover tokens ("2 3", or an unexpected ')' later) are an error at
    // the first token the expression did not consume.
    if (!p.failed && p.tok.kind != T_EOL) fail(&p, p.tok.pos);

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

// Reads the whole file once. Everything else works on offsets into this one
// buffer, so the lines are never copied or split into separate strings.
static char *read_file(const char *path, size_t *len_out) {
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

// "dir/task1.txt" -> "task1"
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

// Creates out_dir if missing; an existing directory is fine.
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
        if (i < le) { // blank lines produce no output
            long long v;
            if (eval_line(src, ls, le, &v, &err_pos)) {
                char tmp[32];
                int n = snprintf(tmp, sizeof tmp, "%lld\n", v);
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
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *out_dir = NULL;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            out_dir = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        } else {
            input = argv[i];
        }
    }

    // Stage 1 only supports "calc -o OUTDIR input.txt".
    if (!input || !out_dir) { usage(); return 2; }
    if (!ensure_dir(out_dir)) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n", out_dir,
                strerror(errno));
        return 1;
    }
    return process_file(input, out_dir);
}