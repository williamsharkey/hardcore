/* Browser captions share the terminal's cell widths.  Keep page identity first:
   a row of tabs for one website must still distinguish the individual pages. */
#ifndef HARDCORE_BROWSER_LABEL_H
#define HARDCORE_BROWSER_LABEL_H

static uint32_t browser_label_scalar(NSString *s,NSUInteger *i) {
    unichar a=[s characterAtIndex:(*i)++];
    if(a>=0xd800 && a<=0xdbff && *i<s.length) {
        unichar b=[s characterAtIndex:*i];
        if(b>=0xdc00 && b<=0xdfff) { (*i)++; return 0x10000+((a-0xd800)<<10)+(b-0xdc00); }
    }
    return a>=0xd800 && a<=0xdfff ? 0xfffd : a;
}
static int browser_label_width(NSString *s) {
    int width=0;
    for(NSUInteger i=0;i<s.length;) width+=cp_width(browser_label_scalar(s,&i));
    return width;
}
static NSString *browser_label_clean(NSString *s) {
    if(![s isKindOfClass:NSString.class])return @"";
    /* Page titles are untrusted DOM strings. Bound work and make them one line. */
    if(s.length>4096) {
        NSUInteger end=4096;
        unichar a=[s characterAtIndex:end-1];
        if(a>=0xd800 && a<=0xdbff)end--;
        s=[s substringToIndex:end];
    }
    NSMutableString *out=[NSMutableString string];
    BOOL space=NO;
    for(NSUInteger i=0;i<s.length;) {
        uint32_t cp=browser_label_scalar(s,&i);
        if(cp<=0x20 || cp==0x7f || cp==0xa0 || cp==0x2028 || cp==0x2029) { space=out.length>0;continue; }
        if((cp>=0x80 && cp<0xa0) || (cp>=0x202a && cp<=0x202e) || (cp>=0x2066 && cp<=0x2069))continue;
        if(space) { [out appendString:@" "];space=NO; }
        if(cp>0xffff) {
            cp-=0x10000;unichar pair[2]={(unichar)(0xd800+(cp>>10)),(unichar)(0xdc00+(cp&1023))};
            [out appendString:[NSString stringWithCharacters:pair length:2]];
        } else { unichar ch=(unichar)cp;[out appendString:[NSString stringWithCharacters:&ch length:1]]; }
    }
    return out;
}
static BOOL browser_label_boundary(unichar c) {
    return c==' ' || c=='/' || c=='\\' || c=='-' || c=='_' || c==':' || c=='.' || c=='|' || c==0x2013 || c==0x2014;
}
static NSString *browser_label_head(NSString *s,int cells,BOOL words) {
    NSUInteger end=0,last=0;int width=0;
    for(NSUInteger i=0;i<s.length;) {
        NSUInteger before=i;uint32_t cp=browser_label_scalar(s,&i);
        int w=cp_width(cp);if(width+w>cells)break;
        width+=w;end=i;
        if(cp<=0xffff && browser_label_boundary((unichar)cp))last=before;
    }
    if(words && end<s.length && last>0 && browser_label_width([s substringToIndex:last])>=cells/2)end=last;
    while(end>0 && browser_label_boundary([s characterAtIndex:end-1]))end--;
    return [s substringToIndex:end];
}
static NSString *browser_label_tail(NSString *s,int cells,BOOL words) {
    NSUInteger start=s.length;int width=0;
    while(start>0) {
        NSUInteger next=start-1;unichar ch=[s characterAtIndex:next];
        if(ch>=0xdc00 && ch<=0xdfff && next>0) {
            unichar high=[s characterAtIndex:next-1];if(high>=0xd800 && high<=0xdbff)next--;
        }
        NSUInteger i=next;int w=cp_width(browser_label_scalar(s,&i));
        if(width+w>cells)break;start=next;width+=w;
    }
    /* Do not start with a combining mark whose base fell outside the budget. */
    while(start<s.length) { NSUInteger i=start;if(cp_width(browser_label_scalar(s,&i)))break;start=i; }
    if(words && start>0 && !browser_label_boundary([s characterAtIndex:start-1])) {
        for(NSUInteger i=start;i<s.length;i++)if(browser_label_boundary([s characterAtIndex:i])) {
            NSUInteger next=i+1;
            if(browser_label_width([s substringFromIndex:next])>=cells*2/3)start=next;
            break;
        }
    }
    return [[s substringFromIndex:start] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
}
static NSString *browser_label_shorten(NSString *s,int cells) {
    if(cells<=0)return @"";
    if(browser_label_width(s)<=cells)return s;
    if(cells==1)return @"…";
    /* Keep both ends of long document titles: version, chapter and language
       often appear at the end. Short labels retain a recognizable beginning. */
    if(cells<10)return [browser_label_head(s,cells-1,YES) stringByAppendingString:@"…"];
    int tail=(cells-1)*2/5,head=cells-1-tail;
    NSRange word=[s rangeOfString:@" " options:NSBackwardsSearch];
    int lastWord=browser_label_width(word.location==NSNotFound ? s : [s substringFromIndex:NSMaxRange(word)]);
    if(cells<18 && lastWord>tail)return [browser_label_head(s,cells-1,YES) stringByAppendingString:@"…"];
    if(lastWord>tail && lastWord<=(cells-1)*2/3) { tail=lastWord;head=cells-1-tail; }
    NSString *left=browser_label_head(s,head,YES),*right=browser_label_tail(s,tail,YES);
    return [NSString stringWithFormat:@"%@…%@",left,right];
}
static BOOL browser_label_host_family(NSString *host,NSString *domain) {
    return [host isEqualToString:domain] || [host hasSuffix:[@"." stringByAppendingString:domain]];
}
static NSString *browser_label_alias(NSString *host) {
    if(browser_label_host_family(host,@"wikipedia.org"))return @"Wiki";
    if(browser_label_host_family(host,@"github.com"))return @"GitHub";
    if(browser_label_host_family(host,@"stackoverflow.com"))return @"StackOverflow";
    if(browser_label_host_family(host,@"youtube.com"))return @"YouTube";
    if(browser_label_host_family(host,@"developer.mozilla.org"))return @"MDN";
    return @"";
}
static BOOL browser_label_brand(NSString *s,NSArray *brands) {
    for(NSString *brand in brands)if(brand.length && [s caseInsensitiveCompare:brand]==NSOrderedSame)return YES;
    return NO;
}
static NSString *browser_label_remove_brand(NSString *title,NSArray *brands) {
    NSArray *separators=@[@" | ",@" — ",@" – ",@" - ",@" · ",@" :: "];
    for(int pass=0;pass<3;pass++) {
        BOOL removed=NO;
        for(NSString *separator in separators) {
            NSRange last=[title rangeOfString:separator options:NSBackwardsSearch];
            if(last.location!=NSNotFound && browser_label_brand([title substringFromIndex:NSMaxRange(last)],brands)) {
                title=[title substringToIndex:last.location];removed=YES;break;
            }
            NSRange first=[title rangeOfString:separator];
            if(first.location!=NSNotFound && browser_label_brand([title substringToIndex:first.location],brands)) {
                title=[title substringFromIndex:NSMaxRange(first)];removed=YES;break;
            }
        }
        if(!removed)break;
    }
    return title;
}
static NSString *browser_label_format(NSString *url,NSString *title,NSString *site,int columns,int child) {
    if(columns<=0)return @"";
    NSString *prefix=child && columns>=8 ? @"↳ " : @"";
    int available=columns-browser_label_width(prefix);
    NSURL *u=[NSURL URLWithString:browser_label_clean(url)];
    NSString *host=u.host.lowercaseString ?: @"";
    if([host hasPrefix:@"www."])host=[host substringFromIndex:4];
    NSString *alias=browser_label_alias(host);
    NSString *authority=host;
    if(u.port)authority=[NSString stringWithFormat:@"%@:%@",host,u.port];
    NSString *compact=alias.length && !u.port ? alias : authority;
    if(!alias.length && !u.port) {
        for(NSString *suffix in @[@".com",@".org",@".net",@".io",@".dev",@".co.uk"])if([compact hasSuffix:suffix]) {
            compact=[compact substringToIndex:compact.length-suffix.length];break;
        }
    }
    NSMutableArray *brands=[NSMutableArray arrayWithObjects:browser_label_clean(site),authority,host,compact,alias,nil];
    if(browser_label_host_family(host,@"wikipedia.org")) {
        [brands addObject:@"Wikipedia"];[brands addObject:@"Wikipedia, the free encyclopedia"];
    }
    NSString *page=browser_label_remove_brand(browser_label_clean(title),brands);
    if(!page.length || browser_label_brand(page,brands)) {
        NSString *path=u.path ?: @"";
        NSArray *parts=[path pathComponents];
        NSString *leaf=parts.lastObject ?: @"";
        /* Repository roots are meaningful as owner/project, not just a generic
           final component such as issues, pull, settings or documentation. */
        if(browser_label_host_family(host,@"github.com") && parts.count>=3)
            leaf=[NSString stringWithFormat:@"%@/%@",parts[1],parts[2]];
        if([@[@"index.html",@"index.htm",@"index.php"] containsObject:leaf.lowercaseString])leaf=parts.count>2 ? parts[parts.count-2] : @"";
        if([leaf isEqualToString:@"/"])leaf=@"";
        leaf=[leaf stringByRemovingPercentEncoding] ?: leaf;
        page=browser_label_clean([leaf stringByReplacingOccurrencesOfString:@"_" withString:@" "]);
        if(!page.length)page=authority.length ? authority : [u.scheme isEqualToString:@"file"] ? @"Local page" : @"Browser";
    }
    NSString *label=page;
    if(authority.length && [page caseInsensitiveCompare:authority]!=NSOrderedSame && !browser_label_brand(page,@[compact,alias])) {
        NSString *full=[NSString stringWithFormat:@"%@ · %@",page,authority];
        if(browser_label_width(full)<=available)label=full;
        else {
            int pageWidth=browser_label_width(page),hostWidth=browser_label_width(compact);
            if(pageWidth+3+hostWidth<=available)label=[NSString stringWithFormat:@"%@ · %@",page,compact];
            else if(available>=16) {
                /* The page gets at least half the row. Long custom hosts retain
                   both ends; localhost's port is kept at the distinctive end. */
                int hostBudget=available/3;
                NSString *portSuffix=u.port ? [NSString stringWithFormat:@":%@",u.port] : @"";
                if(portSuffix.length && hostBudget<browser_label_width(portSuffix)+2)hostBudget=browser_label_width(portSuffix)+2;
                if(hostWidth<hostBudget)hostBudget=hostWidth;
                NSString *shortHost;
                if(portSuffix.length && hostWidth>hostBudget) {
                    NSString *start=browser_label_head(host,hostBudget-browser_label_width(portSuffix)-1,NO);
                    shortHost=[NSString stringWithFormat:@"%@…%@",start,portSuffix];
                } else shortHost=browser_label_shorten(compact,hostBudget);
                NSString *shortPage=browser_label_shorten(page,available-3-browser_label_width(shortHost));
                label=[NSString stringWithFormat:@"%@ · %@",shortPage,shortHost];
            } else label=browser_label_shorten(page,available);
        }
    } else label=browser_label_shorten(label,available);
    return [prefix stringByAppendingString:label];
}

static int browser_label_unit(FILE *f) {
    int failures=0;
    NSString *ada=browser_label_format(@"https://en.wikipedia.org/wiki/Ada_Lovelace",@"Ada Lovelace - Wikipedia",@"Wikipedia",21,0);
    failures+=![ada isEqualToString:@"Ada Lovelace · Wiki"];
    NSString *wide=browser_label_format(@"https://en.wikipedia.org/wiki/Ada_Lovelace",@"Ada Lovelace - Wikipedia",@"Wikipedia",200,0);
    failures+=![wide isEqualToString:@"Ada Lovelace · en.wikipedia.org"];
    failures+=![browser_label_format(@"https://example.org",@"Example | Distinctive Page | Example",@"Example",200,0) isEqualToString:@"Distinctive Page · example.org"];
    NSString *fallback=browser_label_format(@"https://alice:secret@www.example.org/wiki/Ada_Lovelace?token=private#secret-fragment",@"",@"",200,0);
    failures+=![fallback isEqualToString:@"Ada Lovelace · example.org"];
    failures+=[fallback containsString:@"secret"] || [fallback containsString:@"private"] || [fallback containsString:@"alice"];
    failures+=![browser_label_format(@"http://localhost:4321/",@"",@"",22,0) isEqualToString:@"localhost:4321"];
    failures+=![browser_label_format(@"file:///tmp/Report_2026.html",@"",@"",40,0) isEqualToString:@"Report 2026.html"];
    failures+=![browser_label_format(@"https://github.com/octocat/Hello-World/issues?token=secret",@"",@"",80,0) isEqualToString:@"octocat/Hello-World · github.com"];
    failures+=![browser_label_format(@"",@"",@"",20,0) isEqualToString:@"Browser"];
    NSString *a=browser_label_format(@"https://en.wikipedia.org/wiki/Ada_Lovelace",@"Ada Lovelace - Wikipedia",@"Wikipedia",23,1);
    NSString *b=browser_label_format(@"https://en.wikipedia.org/wiki/Alan_Turing",@"Alan Turing - Wikipedia",@"Wikipedia",23,1);
    failures+=[a isEqualToString:b];
    NSArray *titles=@[@"Ada Lovelace - Wikipedia",@"日本語の歴史と文化について 🌏 — Wikipedia",@"A very long document explaining the modern browser architecture — Volume Three",@"Cafe\u0301 and 🧑‍💻 development",@"\t Leading\nspace\r\n\u202eUnsafe\u202c"];
    for(NSString *s in titles)for(int n=1;n<=80;n++)for(int child=0;child<=1;child++) {
        NSString *label=browser_label_format(@"https://docs.example.org/reference/api?key=secret",s,@"Wikipedia",n,child);
        failures+=!label.length || browser_label_width(label)>n;
        for(NSUInteger i=0;i<label.length;) {
            uint32_t cp=browser_label_scalar(label,&i);failures+=cp==0xfffd || cp<0x20 || (cp>=0x7f && cp<0xa0);
        }
    }
    failures+=browser_label_format(@"https://example.org",@"Title",@"",0,1).length!=0;
    NSString *longName=browser_label_format(@"",@"A long history of scientific discoveries — Chapter Seven",@"",24,0);
    failures+=![longName containsString:@"…"] || ![longName hasSuffix:@"Seven"];
    fprintf(f,"  %s browser captions: page identity, branding, URL privacy, Unicode and 1–80 cell budgets\n",failures ? "FAIL" : "ok");
    return failures;
}
#endif
