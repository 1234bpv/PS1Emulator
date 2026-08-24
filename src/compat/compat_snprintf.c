// snprintf/vsnprintf for Symbian: ESTLIB lacks them (this is C99, while ESTLIB is from the C89 era),
// and they are not exported by any SDK library - checked across all .dso files.
// libpcsxcore uses them actively, so without this neither the core nor the shell will link.
//
// Bounded formatting is implemented directly rather than calling vsprintf into a large buffer:
// the latter silently overflows the stack on the very first long string.
// Supported format specifiers encountered in the codebase:
// %s %c %d %i %u %x %X %p %% with flags -, 0, width, precision, and lengths l/ll/z.

#include <stdarg.h>
#include <stddef.h>

struct sink {
	char *buf;
	size_t cap;   // capacity including null terminator; 0 - count only
	size_t len;   // how many characters full formatting would emit (C99 semantics)
};

static void emit(struct sink *s, char c)
{
	if (s->cap && s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void emit_pad(struct sink *s, char c, int n)
{
	while (n-- > 0)
		emit(s, c);
}

// Emits the formatted body of a number/string respecting width and alignment.
static void emit_field(struct sink *s, const char *body, int bodylen,
                       const char *prefix, int prefixlen,
                       int width, int leftalign, int zeropad)
{
	int pad = width - bodylen - prefixlen;
	int i;

	if (pad > 0 && !leftalign && !zeropad)
		emit_pad(s, ' ', pad);
	for (i = 0; i < prefixlen; i++)
		emit(s, prefix[i]);
	if (pad > 0 && !leftalign && zeropad)
		emit_pad(s, '0', pad);
	for (i = 0; i < bodylen; i++)
		emit(s, body[i]);
	if (pad > 0 && leftalign)
		emit_pad(s, ' ', pad);
}

static int utoa_rev(unsigned long long v, unsigned base, int upper, char *out)
{
	static const char lo[] = "0123456789abcdef";
	static const char up[] = "0123456789ABCDEF";
	const char *digits = upper ? up : lo;
	int n = 0;

	if (v == 0)
		out[n++] = '0';
	while (v) {
		out[n++] = digits[v % base];
		v /= base;
	}
	// Reverse in place.
	{
		int i;
		for (i = 0; i < n / 2; i++) {
			char t = out[i];
			out[i] = out[n - 1 - i];
			out[n - 1 - i] = t;
		}
	}
	return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct sink s;
	char num[32];

	s.buf = buf;
	s.cap = size;
	s.len = 0;

	while (*fmt) {
		int leftalign = 0, zeropad = 0, plus = 0, space = 0, alt = 0;
		int width = 0, prec = -1, longs = 0, sizet = 0;
		char conv;

		if (*fmt != '%') {
			emit(&s, *fmt++);
			continue;
		}
		fmt++;

		for (;;) {
			if      (*fmt == '-') { leftalign = 1; fmt++; }
			else if (*fmt == '0') { zeropad = 1;   fmt++; }
			else if (*fmt == '+') { plus = 1;      fmt++; }
			else if (*fmt == ' ') { space = 1;     fmt++; }
			else if (*fmt == '#') { alt = 1;       fmt++; }
			else break;
		}

		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) { leftalign = 1; width = -width; }
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}

		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
			if (prec < 0)
				prec = -1;
		}

		while (*fmt == 'l') { longs++; fmt++; }
		if (*fmt == 'z') { sizet = 1; fmt++; }
		if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }

		conv = *fmt ? *fmt++ : 0;
		if (!conv)
			break;

		switch (conv) {
		case '%':
			emit(&s, '%');
			break;

		case 'c':
			{
				char c = (char)va_arg(ap, int);
				emit_field(&s, &c, 1, "", 0, width, leftalign, 0);
			}
			break;

		case 's':
			{
				const char *p = va_arg(ap, const char *);
				int n = 0;
				if (!p)
					p = "(null)";
				while (p[n] && (prec < 0 || n < prec))
					n++;
				emit_field(&s, p, n, "", 0, width, leftalign, 0);
			}
			break;

		case 'd':
		case 'i':
			{
				long long v = sizet ? (long long)va_arg(ap, size_t)
				            : longs >= 2 ? va_arg(ap, long long)
				            : longs == 1 ? (long long)va_arg(ap, long)
				                         : (long long)va_arg(ap, int);
				unsigned long long mag = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL
				                              : (unsigned long long)v;
				int n = utoa_rev(mag, 10, 0, num);
				const char *pfx = v < 0 ? "-" : plus ? "+" : space ? " " : "";
				emit_field(&s, num, n, pfx, pfx[0] ? 1 : 0, width, leftalign, zeropad);
			}
			break;

		case 'u':
		case 'x':
		case 'X':
		case 'o':
			{
				unsigned base = conv == 'o' ? 8 : (conv == 'u' ? 10 : 16);
				unsigned long long v = sizet ? (unsigned long long)va_arg(ap, size_t)
				                     : longs >= 2 ? va_arg(ap, unsigned long long)
				                     : longs == 1 ? (unsigned long long)va_arg(ap, unsigned long)
				                                  : (unsigned long long)va_arg(ap, unsigned int);
				int n = utoa_rev(v, base, conv == 'X', num);
				const char *pfx = (alt && base == 16) ? (conv == 'X' ? "0X" : "0x") : "";
				emit_field(&s, num, n, pfx, pfx[0] ? 2 : 0, width, leftalign, zeropad);
			}
			break;

		case 'p':
			{
				unsigned long v = (unsigned long)(size_t)va_arg(ap, void *);
				int n = utoa_rev(v, 16, 0, num);
				emit_field(&s, num, n, "0x", 2, width, leftalign, zeropad);
			}
			break;

		default:
			// Output unknown specifiers verbatim to avoid losing text.
			emit(&s, '%');
			emit(&s, conv);
			break;
		}
	}

	if (s.cap)
		s.buf[s.len < s.cap ? s.len : s.cap - 1] = '\0';
	return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

// fseeko/ftello - POSIX variants for large files; ESTLIB lacks them.
// There off_t is defined as long (sys/types.h:55), so these are direct equivalents.

#include <stdio.h>
#include <sys/types.h>

int fseeko(FILE *stream, off_t offset, int whence)
{
	return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream)
{
	return (off_t)ftell(stream);
}
