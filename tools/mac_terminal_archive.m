/* Experimental Terminal styled-history archive. No Terminal automation.
 * Only --capture reads the pasteboard; --selftest and --decode are offline.
 * Normalized text is display data, never terminal input or an escape stream. */
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_BYTES = 8 * 1024 * 1024, MAX_CHARS = 1024 * 1024, MAX_RUNS = 100000 };

static BOOL fail(NSString **why, NSString *message) {
    if (why) *why = message;
    return NO;
}

static BOOL validToken(NSString *token) {
    if (token.length != 32) return NO;
    return [token rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:
        @"0123456789abcdef"] invertedSet]].location == NSNotFound;
}

static BOOL lineBoundary(unichar c) {
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static BOOL markers(NSString *text, NSString *token) {
    NSString *begin = [NSString stringWithFormat:@"HCTERM-FIXTURE:%@:BEGIN", token];
    NSString *end = [NSString stringWithFormat:@"HCTERM-FIXTURE:%@:END", token];
    NSRange a = [text rangeOfString:begin], b = [text rangeOfString:end];
    if (a.location == NSNotFound || b.location == NSNotFound || NSMaxRange(a) >= b.location)
        return NO;
    if ((a.location && !lineBoundary([text characterAtIndex:a.location - 1])) ||
        (NSMaxRange(a) < text.length && !lineBoundary([text characterAtIndex:NSMaxRange(a)])) ||
        (b.location && !lineBoundary([text characterAtIndex:b.location - 1])) ||
        (NSMaxRange(b) < text.length && !lineBoundary([text characterAtIndex:NSMaxRange(b)])))
        return NO;
    return [text rangeOfString:begin options:0 range:NSMakeRange(NSMaxRange(a), text.length-NSMaxRange(a))].location == NSNotFound &&
           [text rangeOfString:end options:0 range:NSMakeRange(NSMaxRange(b), text.length-NSMaxRange(b))].location == NSNotFound;
}

static NSRange fixtureRange(NSString *text, NSString *token) {
    NSString *begin = [NSString stringWithFormat:@"HCTERM-FIXTURE:%@:BEGIN", token];
    NSString *end = [NSString stringWithFormat:@"HCTERM-FIXTURE:%@:END", token];
    NSRange a = [text rangeOfString:begin], b = [text rangeOfString:end];
    NSUInteger finish = NSMaxRange(b);
    if (finish < text.length && lineBoundary([text characterAtIndex:finish])) {
        unichar first = [text characterAtIndex:finish++];
        if (first == '\r' && finish < text.length && [text characterAtIndex:finish] == '\n') finish++;
    }
    return NSMakeRange(a.location, finish - a.location);
}

static NSDictionary *rangeJSON(NSRange range) {
    return @{@"start": @(range.location), @"length": @(range.length)};
}

static NSDictionary *textComparison(NSString *plain, NSString *rtf, BOOL includeUnits) {
    NSUInteger at = 0, length = MIN(plain.length, rtf.length);
    while (at < length && [plain characterAtIndex:at] == [rtf characterAtIndex:at]) at++;
    BOOL exact = at == plain.length && at == rtf.length;
    NSMutableDictionary *result = [@{@"exact": @(exact), @"plainUTF16Length": @(plain.length),
        @"rtfUTF16Length": @(rtf.length), @"firstMismatchUTF16": exact ? (id)[NSNull null] : @(at)} mutableCopy];
    if (includeUnits && !exact) {
        NSMutableArray *a = [NSMutableArray array], *b = [NSMutableArray array];
        for (NSUInteger i = at; i < MIN(at + 8, plain.length); i++)
            [a addObject:[NSString stringWithFormat:@"%04X", [plain characterAtIndex:i]]];
        for (NSUInteger i = at; i < MIN(at + 8, rtf.length); i++)
            [b addObject:[NSString stringWithFormat:@"%04X", [rtf characterAtIndex:i]]];
        result[@"plainCodeUnits"] = a; result[@"rtfCodeUnits"] = b;
    }
    return result;
}

static NSString *lineEndings(NSString *text) {
    return [[text stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]
                   stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
}

static BOOL literalEqual(NSString *a, NSString *b) {
    return [textComparison(a, b, NO)[@"exact"] boolValue];
}

/* One UTF-16 unit in, one unit out: attribute ranges stay valid. */
static NSString *displayText(NSString *text, NSUInteger *changed) {
    NSMutableString *result = [text mutableCopy];
    NSUInteger count = 0;
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i], replacement = c;
        if (c < 0x20 && c != '\n' && c != '\t') replacement = 0x2400 + c;
        else if (c == 0x7f) replacement = 0x2421;
        else if ((c >= 0x80 && c <= 0x9f) || (c >= 0x202a && c <= 0x202e) ||
                 (c >= 0x2066 && c <= 0x2069)) replacement = 0xfffd;
        if (replacement != c) {
            [result replaceCharactersInRange:NSMakeRange(i, 1)
                                  withString:[NSString stringWithCharacters:&replacement length:1]];
            count++;
        }
    }
    if (changed) *changed = count;
    return result;
}

static NSArray *rgba(id value) {
    if (![value isKindOfClass:[NSColor class]]) return nil;
    NSColor *c = [value colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!c) return nil;
    CGFloat r, g, b, a;
    [c getRed:&r green:&g blue:&b alpha:&a];
    if (!isfinite(r) || !isfinite(g) || !isfinite(b) || !isfinite(a)) return nil;
    return @[@(fmax(0, fmin(1, r))), @(fmax(0, fmin(1, g))),
             @(fmax(0, fmin(1, b))), @(fmax(0, fmin(1, a)))];
}

/* Preserve the plain copy literally. RTF exporters can compose Unicode, so
 * attribute offsets cannot be copied across the two strings unchanged.
 * Only canonically equivalent, one-to-one grapheme clusters are eligible.
 * A changed cluster must have one uniform RTF attribute set; differing styles
 * inside it have no unambiguous mapping and are refused. */
static NSAttributedString *stylesOnPlain(NSAttributedString *rtf, NSString *plain,
                                        NSUInteger *remapped, NSString **why) {
    *remapped = 0;
    if (literalEqual(plain, rtf.string)) return rtf;
    if (!literalEqual(plain.precomposedStringWithCanonicalMapping,
                      rtf.string.precomposedStringWithCanonicalMapping)) {
        fail(why, @"RTF and plain fixture text differ beyond canonical Unicode composition"); return nil;
    }
    NSMutableAttributedString *mapped = [[NSMutableAttributedString alloc] initWithString:@""];
    NSUInteger p = 0, r = 0;
    while (p < plain.length && r < rtf.length) {
        NSRange pr = [plain rangeOfComposedCharacterSequenceAtIndex:p];
        NSRange rr = [rtf.string rangeOfComposedCharacterSequenceAtIndex:r];
        if (pr.location != p || rr.location != r || !pr.length || !rr.length) {
            fail(why, @"ambiguous Unicode grapheme boundaries"); return nil;
        }
        NSString *pt = [plain substringWithRange:pr], *rt = [rtf.string substringWithRange:rr];
        if (literalEqual(pt, rt)) {
            [mapped appendAttributedString:[rtf attributedSubstringFromRange:rr]];
        } else {
            if (!literalEqual(pt.precomposedStringWithCanonicalMapping, rt.precomposedStringWithCanonicalMapping)) {
                fail(why, @"Unicode graphemes do not correspond one to one"); return nil;
            }
            NSRange effective;
            NSDictionary *attributes = [rtf attributesAtIndex:r longestEffectiveRange:&effective inRange:rr];
            if (!NSEqualRanges(effective, rr)) {
                fail(why, @"cannot map differing styles inside a Unicode-composed grapheme"); return nil;
            }
            [mapped appendAttributedString:[[NSAttributedString alloc] initWithString:pt attributes:attributes]];
            (*remapped)++;
        }
        p = NSMaxRange(pr); r = NSMaxRange(rr);
    }
    if (p != plain.length || r != rtf.length || !literalEqual(mapped.string, plain)) {
        fail(why, @"Unicode style mapping did not preserve the exact plain text"); return nil;
    }
    return mapped;
}

static NSDictionary *normalize(NSAttributedString *value, NSString *plain,
                               NSString *token, NSString **why, NSDictionary **comparison) {
    if (!validToken(token)) { fail(why, @"fixture token must be 32 lowercase hex characters"); return nil; }
    if (plain.length > MAX_CHARS || value.length > MAX_CHARS) {
        fail(why, @"text exceeds the one-million UTF-16-unit limit"); return nil;
    }
    if (!markers(plain, token) || !markers(value.string, token)) {
        fail(why, @"exact, unique fixture begin/end markers are missing or malformed"); return nil;
    }
    NSRange plainRange = fixtureRange(plain, token), rtfRange = fixtureRange(value.string, token);
    NSString *plainFixture = [plain substringWithRange:plainRange];
    NSString *rtfFixture = [value.string substringWithRange:rtfRange];
    NSMutableDictionary *fixture = [textComparison(plainFixture, rtfFixture, YES) mutableCopy];
    fixture[@"plainRange"] = rangeJSON(plainRange);
    fixture[@"rtfRange"] = rangeJSON(rtfRange);
    BOOL exact = [fixture[@"exact"] boolValue];
    // These hints never establish literal equality. Canonical composition may
    // be reconciled only by separately verified style mapping onto exact plain.
    fixture[@"diagnosticEquivalence"] = @{
        @"lineEndingsOnly": @((BOOL)(!exact && literalEqual(lineEndings(plainFixture), lineEndings(rtfFixture)))),
        @"unicodeNormalizationOnly": @((BOOL)(!exact && literalEqual(plainFixture.precomposedStringWithCanonicalMapping,
                                                            rtfFixture.precomposedStringWithCanonicalMapping))),
        @"lineEndingsAndUnicodeNormalization": @((BOOL)(!exact && literalEqual(lineEndings(plainFixture).precomposedStringWithCanonicalMapping,
                                                                      lineEndings(rtfFixture).precomposedStringWithCanonicalMapping)))};
    NSDictionary *report = @{@"format": @"hardcore-terminal-text-comparison-v1", @"fixtureToken": token,
        @"scope": @"fixture", @"fullTerminalState": @NO,
        @"wholeSelection": textComparison(plain, value.string, NO), @"fixture": fixture};
    if (comparison) *comparison = report;
    /* Ranges are independent: unrelated prefixes can have different lengths. */
    value = [value attributedSubstringFromRange:rtfRange];
    plain = plainFixture;
    NSUInteger remapped = 0;
    value = stylesOnPlain(value, plain, &remapped, why);
    if (!value) return nil;
    NSMutableArray *runs = [NSMutableArray array];
    NSMutableSet *unknown = [NSMutableSet set], *observed = [NSMutableSet set];
    __block BOOL rejected = NO;
    [value enumerateAttributesInRange:NSMakeRange(0, value.length) options:0
                          usingBlock:^(NSDictionary *attrs, NSRange range, BOOL *stop) {
        if (runs.count >= MAX_RUNS) { rejected = YES; *stop = YES; return; }
        if (attrs[NSAttachmentAttributeName]) { rejected = YES; *stop = YES; return; }
        NSMutableDictionary *run = [@{@"start": @(range.location), @"length": @(range.length)} mutableCopy];
        for (NSString *key in attrs) {
            id item = attrs[key];
            if ([key isEqual:NSForegroundColorAttributeName] || [key isEqual:NSBackgroundColorAttributeName]) {
                NSArray *color = rgba(item);
                if (!color) { rejected = YES; *stop = YES; return; }
                NSString *field = [key isEqual:NSForegroundColorAttributeName] ? @"foreground" : @"background";
                run[field] = color; [observed addObject:field];
            } else if ([key isEqual:NSFontAttributeName] && [item isKindOfClass:[NSFont class]]) {
                NSFont *font = item;
                if (!isfinite(font.pointSize) || font.pointSize <= 0 || font.pointSize > 512) {
                    rejected = YES; *stop = YES; return;
                }
                NSFontSymbolicTraits traits = font.fontDescriptor.symbolicTraits;
                run[@"font"] = @{@"name": font.fontName ?: @"", @"size": @(font.pointSize),
                                 @"bold": @((traits & NSFontBoldTrait) != 0),
                                 @"italic": @((traits & NSFontItalicTrait) != 0)};
                [observed addObject:@"font"];
            } else if ([key isEqual:NSUnderlineStyleAttributeName] || [key isEqual:NSStrikethroughStyleAttributeName]) {
                if (![item isKindOfClass:[NSNumber class]]) { rejected = YES; *stop = YES; return; }
                NSString *field = [key isEqual:NSUnderlineStyleAttributeName] ? @"underline" : @"strikethrough";
                run[field] = @([item integerValue]); [observed addObject:field];
            } else if ([key isEqual:NSLinkAttributeName]) {
                NSString *link = [item isKindOfClass:[NSURL class]] ? [item absoluteString] : item;
                if (![link isKindOfClass:[NSString class]] || link.length > 4096) {
                    rejected = YES; *stop = YES; return;
                }
                run[@"link"] = @{@"text": displayText(link, NULL), @"actionable": @NO};
                [observed addObject:@"link"];
            } else {
                [unknown addObject:key];
            }
        }
        [runs addObject:run];
    }];
    if (rejected) { fail(why, @"unsupported attachment, invalid attribute, or too many style runs"); return nil; }
    NSUInteger changed = 0;
    NSString *safe = displayText(plain, &changed);
    return @{@"format": @"hardcore-terminal-styled-archive-v1", @"fixtureToken": token,
             @"text": safe, @"rangeUnits": @"UTF-16", @"colorSpace": @"sRGB", @"runs": runs,
             @"originalRange": rangeJSON(plainRange), @"rtfOriginalRange": rangeJSON(rtfRange),
             @"comparison": report,
             @"styleMapping": @{@"method": remapped ? @"canonical-grapheme" : @"literal",
                 @"verified": @YES, @"plainTextPreserved": @((BOOL)(changed == 0)),
                 @"remappedClusters": @(remapped), @"textSource": @"plain", @"styleSource": @"RTF",
                 @"scope": @"available clipboard representations; not proof of original per-cell style"},
             @"sanitizedCodeUnits": @(changed), @"fullTerminalState": @NO,
             @"observedAttributes": [[observed allObjects] sortedArrayUsingSelector:@selector(compare:)],
             @"unrepresentedAttributes": [[unknown allObjects] sortedArrayUsingSelector:@selector(compare:)],
             @"missingState": @[@"cursor", @"soft-wrap flags", @"alternate screen", @"emulator modes",
                                @"parser state", @"shell state", @"running jobs"],
             @"usage": @"Display archive only. Never send text or RTF to a PTY or escape-sequence parser."};
}

static NSDictionary *decode(NSData *rtf, NSData *plainData, NSString *token, NSString **why,
                            NSDictionary **comparison, NSString **decodedText) {
    if (!rtf || !plainData || rtf.length > MAX_BYTES || plainData.length > MAX_BYTES) {
        fail(why, @"missing data or input exceeds the eight-MiB limit"); return nil;
    }
    NSString *plain = [[NSString alloc] initWithData:plainData encoding:NSUTF8StringEncoding];
    if (!plain || plain.length > MAX_CHARS || !validToken(token) || !markers(plain, token)) {
        fail(why, @"invalid UTF-8 text, size, or fixture identity"); return nil;
    }
    const char prefix[] = "{\\rtf";
    if (rtf.length < sizeof(prefix)-1 || memcmp(rtf.bytes, prefix, sizeof(prefix)-1)) {
        fail(why, @"input is not an RTF document"); return nil;
    }
    int depth = 0;
    const unsigned char *bytes = rtf.bytes;
    for (NSUInteger i = 0; i < rtf.length; i++) {
        if (bytes[i] == '\\' && i + 1 < rtf.length) {
            unsigned char next = bytes[i + 1];
            if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z')) {
                NSUInteger start = ++i;
                while (i + 1 < rtf.length &&
                       ((bytes[i + 1] >= 'A' && bytes[i + 1] <= 'Z') ||
                        (bytes[i + 1] >= 'a' && bytes[i + 1] <= 'z'))) i++;
                NSUInteger length = i - start + 1;
                /* Recognize actual control words, not the second slash in
                 * an escaped literal such as \\pict. Numeric parameters and
                 * every nonletter end a word; case does not bypass refusal. */
                if ((length == 6 && !strncasecmp((const char *)bytes + start, "object", length)) ||
                    (length == 7 && !strncasecmp((const char *)bytes + start, "objdata", length)) ||
                    (length == 4 && !strncasecmp((const char *)bytes + start, "pict", length)) ||
                    (length == 3 && !strncasecmp((const char *)bytes + start, "bin", length))) {
                    fail(why, @"RTF objects, images, and binary payloads are unsupported"); return nil;
                }
            } else {
                /* All control symbols consume their following byte. In
                 * particular, escaped backslashes and braces are text. */
                i++;
            }
            continue;
        }
        if (bytes[i] == '{' && ++depth > 128) { fail(why, @"RTF nesting exceeds limit"); return nil; }
        if (bytes[i] == '}' && --depth < 0) { fail(why, @"RTF braces are unbalanced"); return nil; }
    }
    if (depth) { fail(why, @"RTF braces are unbalanced"); return nil; }
    @try {
        NSAttributedString *value = [[NSAttributedString alloc] initWithRTF:rtf documentAttributes:NULL];
        if (!value) { fail(why, @"RTF could not be decoded"); return nil; }
        NSDictionary *archive = normalize(value, plain, token, why, comparison);
        if (decodedText && value.length <= MAX_CHARS) *decodedText = value.string;
        return archive;
    } @catch (NSException *exception) {
        (void)exception;
        fail(why, @"RTF decoder rejected the document"); return nil;
    }
}

static NSData *boundedFile(NSString *path, NSString **why) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) { fail(why, @"cannot open input file without following symlinks"); return nil; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > MAX_BYTES) {
        close(fd); fail(why, @"input must be a regular file of at most eight MiB"); return nil;
    }
    NSMutableData *data = [NSMutableData data];
    char buf[16384]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (data.length + n > MAX_BYTES) { close(fd); fail(why, @"input grew beyond size limit"); return nil; }
        [data appendBytes:buf length:n];
    }
    close(fd);
    if (n < 0) { fail(why, @"could not read input file"); return nil; }
    return data;
}

static BOOL writeNew(NSString *path, NSData *data, NSString **why) {
    int fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return fail(why, @"output already exists or cannot be created");
    const char *p = data.bytes; NSUInteger left = data.length;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) { close(fd); unlink(path.fileSystemRepresentation); return fail(why, @"output write failed"); }
        p += n; left -= n;
    }
    BOOL ok = fsync(fd) == 0;
    close(fd);
    return ok ? YES : fail(why, @"could not flush output");
}

static BOOL saveCapture(NSString *directory, NSDictionary *archive, NSDictionary *comparison,
                        NSString *decodedText, NSData *rtf, NSData *plain, NSString **why) {
    NSData *json = archive ? [NSJSONSerialization dataWithJSONObject:archive options:NSJSONWritingPrettyPrinted error:NULL] : nil;
    NSData *report = [NSJSONSerialization dataWithJSONObject:comparison options:NSJSONWritingPrettyPrinted error:NULL];
    NSData *decoded = [decodedText dataUsingEncoding:NSUTF8StringEncoding];
    if ((archive && !json) || !report || !decoded) return fail(why, @"cannot encode capture evidence");
    /* Caller chooses a new path; no existing directory or symlink is accepted. */
    if (mkdir(directory.fileSystemRepresentation, 0700))
        return fail(why, @"archive directory must be a new path");
    return writeNew([directory stringByAppendingPathComponent:@"original.rtf"], rtf, why) &&
           writeNew([directory stringByAppendingPathComponent:@"original.txt"], plain, why) &&
           writeNew([directory stringByAppendingPathComponent:@"decoded-rtf.txt"], decoded, why) &&
           writeNew([directory stringByAppendingPathComponent:@"comparison.json"], report, why) &&
           (!archive || writeNew([directory stringByAppendingPathComponent:@"archive.json"], json, why));
}

/* String/data convenience methods on NSPasteboard have different multi-item
 * semantics. Read both representations from one identified item instead. */
static NSDictionary *captureItem(NSArray<NSPasteboardItem *> *items, NSString *token, NSString **why) {
    if (!items.count || items.count > 64) { fail(why, @"clipboard item count is unsupported"); return nil; }
    NSPasteboardItem *selected = nil;
    NSString *plain = nil;
    NSUInteger selectedIndex = 0;
    for (NSUInteger i = 0; i < items.count; i++) {
        NSString *text = [items[i] stringForType:NSPasteboardTypeString];
        if (!text || text.length > MAX_CHARS || !markers(text, token)) continue;
        if (selected) { fail(why, @"multiple clipboard items contain the fixture"); return nil; }
        selected = items[i]; plain = [text copy]; selectedIndex = i;
    }
    if (!selected) { fail(why, @"clipboard does not contain this fixture"); return nil; }
    NSData *rtf = [[selected dataForType:NSPasteboardTypeRTF] copy];
    NSData *text = [plain dataUsingEncoding:NSUTF8StringEncoding];
    if (!rtf || !text || rtf.length > MAX_BYTES || text.length > MAX_BYTES) {
        fail(why, @"fixture item lacks RTF/text or exceeds the eight-MiB limit"); return nil;
    }
    // changeCount alone tracks ownership, not immutable provider data.
    if (!literalEqual(plain, [selected stringForType:NSPasteboardTypeString] ?: @"") ||
        ![rtf isEqualToData:[selected dataForType:NSPasteboardTypeRTF]]) {
        fail(why, @"fixture item changed while its representations were read"); return nil;
    }
    return @{@"rtf": rtf, @"plain": text,
        @"metadata": @{@"itemCount": @(items.count), @"itemIndex": @(selectedIndex),
                         @"pairedRepresentationsFromSameItem": @YES}};
}

static int selftest(void) {
    NSString *token = @"0123456789abcdef0123456789abcdef";
    NSString *plain = [NSString stringWithFormat:@"HCTERM-FIXTURE:%@:BEGIN\ncafé 🚀 red\nHCTERM-FIXTURE:%@:END\n", token, token];
    NSMutableAttributedString *value = [[NSMutableAttributedString alloc] initWithString:plain];
    NSRange red = [plain rangeOfString:@"red"];
    [value addAttributes:@{NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:1 green:0 blue:0 alpha:1],
                          NSBackgroundColorAttributeName: [NSColor colorWithSRGBRed:0 green:0 blue:1 alpha:1],
                          NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                          NSLinkAttributeName: @"javascript:example()"} range:red];
    NSString *why = nil;
    NSDictionary *archive = normalize(value, plain, token, &why, NULL);
    int failures = 0, checks = 0;
#define CHECK(condition, label) do { checks++; if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } } while (0)
    CHECK(archive != nil, "synthetic archive accepted");
    CHECK([archive[@"text"] isEqual:plain], "Unicode text preserved");
    CHECK([archive[@"fullTerminalState"] isEqual:@NO], "never full-state proof");
    NSDictionary *colored = nil;
    for (NSDictionary *r in archive[@"runs"]) if (r[@"foreground"]) colored = r;
    CHECK(([colored[@"foreground"] isEqual:@[@1,@0,@0,@1]]), "foreground RGBA");
    CHECK(([colored[@"background"] isEqual:@[@0,@0,@1,@1]]), "background RGBA");
    CHECK([colored[@"underline"] intValue] == 1, "underline attribute");
    CHECK([colored[@"link"][@"actionable"] isEqual:@NO], "links inert including unsafe schemes");
    CHECK(!normalize(value, plain, @"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &why, NULL), "wrong token refused");
    CHECK(!normalize(value, [plain stringByReplacingOccurrencesOfString:@"red" withString:@"changed"], token, &why, NULL), "fixture text mismatch refused");
    CHECK(!markers([plain stringByAppendingString:plain], token), "duplicate markers refused");
    CHECK(!markers([@"prefix" stringByAppendingString:plain], token), "markers must be full lines");
    NSUInteger changed = 0;
    NSString *unsafe = [NSString stringWithFormat:@"a\033[31m\001\177%C\u202eb\n\t🚀", (unichar)0x9b];
    NSString *safe = displayText(unsafe, &changed);
    CHECK(changed == 5 && [safe hasPrefix:@"a␛[31m␁␡��b"], "control and bidi sanitizer");
    CHECK(safe.length == unsafe.length, "UTF-16 ranges unchanged");
    NSString *huge = [@"x" stringByPaddingToLength:MAX_CHARS + 1 withString:@"x" startingAtIndex:0];
    CHECK(!normalize([[NSAttributedString alloc] initWithString:huge], huge, token, &why, NULL), "character limit");
    NSData *rtf = [value RTFFromRange:NSMakeRange(0, value.length) documentAttributes:@{}];
    CHECK(decode(rtf, [plain dataUsingEncoding:NSUTF8StringEncoding], token, &why, NULL, NULL) != nil, "RTF round trip");
    CHECK(!decode([@"not rtf" dataUsingEncoding:NSUTF8StringEncoding], [plain dataUsingEncoding:NSUTF8StringEncoding], token, &why, NULL, NULL), "invalid RTF rejected");
    NSData *object = [@"{\\rtf1 {\\object hidden}}" dataUsingEncoding:NSUTF8StringEncoding];
    CHECK(!decode(object, [plain dataUsingEncoding:NSUTF8StringEncoding], token, &why, NULL, NULL), "embedded object rejected");
    CHECK(!decode([NSMutableData dataWithLength:MAX_BYTES + 1], [plain dataUsingEncoding:NSUTF8StringEncoding], token, &why, NULL, NULL), "byte limit before decode");
    NSPasteboardItem *unrelated = [[NSPasteboardItem alloc] init], *fixtureItem = [[NSPasteboardItem alloc] init];
    [unrelated setString:@"Unrelated first item" forType:NSPasteboardTypeString];
    [unrelated setData:[@"{\\rtf1 Unrelated first item}" dataUsingEncoding:NSUTF8StringEncoding] forType:NSPasteboardTypeRTF];
    [fixtureItem setString:plain forType:NSPasteboardTypeString];
    [fixtureItem setData:rtf forType:NSPasteboardTypeRTF];
    NSDictionary *pair = captureItem(@[unrelated, fixtureItem], token, &why);
    CHECK([pair[@"rtf"] isEqualToData:rtf] && [pair[@"plain"] isEqualToData:[plain dataUsingEncoding:NSUTF8StringEncoding]], "pair representations from same fixture item");
    CHECK([pair[@"metadata"][@"itemIndex"] intValue] == 1, "selected clipboard item recorded");
    CHECK(!captureItem(@[fixtureItem, fixtureItem], token, &why), "duplicate fixture clipboard items refused");
    CHECK(!captureItem(@[unrelated], token, &why), "unrelated clipboard item refused");
    fprintf(stdout, "%d checks; %d failures. Offline: pasteboard was not read.\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
        BOOL capture = argc == 4 && !strcmp(argv[1], "--capture");
        BOOL offline = argc == 6 && !strcmp(argv[1], "--decode");
        if (!capture && !offline) {
            fprintf(stderr, "Usage: %s --selftest\n       %s --capture TOKEN NEW_DIRECTORY\n       %s --decode RTF_FILE UTF8_TEXT_FILE TOKEN NEW_DIRECTORY\n", argv[0], argv[0], argv[0]);
            return 2;
        }
        NSString *token = @(argv[capture ? 2 : 4]), *directory = @(argv[capture ? 3 : 5]);
        NSString *why = nil;
        NSData *rtf = nil, *plain = nil;
        NSDictionary *captureMetadata = nil;
        if (capture) {
            if (!validToken(token)) { fprintf(stderr, "Invalid fixture token.\n"); return 2; }
            NSPasteboard *board = [NSPasteboard generalPasteboard];
            NSInteger generation = board.changeCount;
            NSDictionary *pair = captureItem(board.pasteboardItems, token, &why);
            if (!pair) { fprintf(stderr, "Capture refused: %s; nothing saved.\n", why.UTF8String); return 1; }
            rtf = pair[@"rtf"]; plain = pair[@"plain"]; captureMetadata = pair[@"metadata"];
            if (board.changeCount != generation) { fprintf(stderr, "Clipboard changed during capture; nothing saved.\n"); return 1; }
        } else {
            rtf = boundedFile(@(argv[2]), &why);
            if (rtf) plain = boundedFile(@(argv[3]), &why);
        }
        NSDictionary *comparison = nil;
        NSString *decodedText = nil;
        NSDictionary *archive = why ? nil : decode(rtf, plain, token, &why, &comparison, &decodedText);
        if (comparison && captureMetadata) {
            NSMutableDictionary *withMetadata = [comparison mutableCopy];
            withMetadata[@"capture"] = captureMetadata; comparison = withMetadata;
            if (archive) {
                NSMutableDictionary *withComparison = [archive mutableCopy];
                withComparison[@"comparison"] = comparison; archive = withComparison;
            }
        }
        // Only bounded, decoded, independently identified fixture data may be
        // retained on refusal. Invalid/ambiguous identity still saves nothing.
        if (comparison && !saveCapture(directory, archive, comparison, decodedText, rtf, plain, &why)) {
            fprintf(stderr, "Capture could not be saved: %s\n", why.UTF8String); return 1;
        }
        if (!archive) {
            if (comparison) fprintf(stderr, "Capture diagnostics saved: %s/comparison.json\n", directory.fileSystemRepresentation);
            fprintf(stderr, "Archive refused: %s\n", (why ?: @"invalid capture").UTF8String); return 1;
        }
        printf("Saved styled archive: %s\nAttributes observed: %s\nFull terminal/shell state is NOT verified; leave Terminal open.\n",
               directory.fileSystemRepresentation, [archive[@"observedAttributes"] description].UTF8String);
        return 0;
    }
}
