/* Live Chrome documents inside a terminal PTY. The browser and projection are
   shared with desktop tabs; this view supplies only terminal presentation. */
#ifndef HARDCORE_HERE_CHROME_H
#define HARDCORE_HERE_CHROME_H
#include "hardcore_chrome_browser.h"
#include "build/browser.inc"

static NSString *hhh_chrome_string(id value) {
    return [value isKindOfClass:NSString.class] ? hhh_safe(value) : @"";
}
static NSString *hhh_chrome_url(NSString *value) {
    if ([value isEqual:@"chrome://history"] || [value isEqual:@"chrome://extensions"] ||
        [value isEqual:@"chrome://settings"]) return value;
    return hhh_url(value,nil);
}

@interface HCHereChrome : NSObject {
@public
    HCChromePage *chrome;
    NSDictionary *snapshot,*control;
    NSArray *actions;
    NSString *text,*status,*address;
    NSMutableString *input;
    BOOL dirty,requesting,again,stopped;
    int revision,prompt,controlNode,controlRevision,offset,cols,viewportCols,viewportRows;
}
- (void)project;
- (void)choose:(int)number;
- (void)applyControl:(id)value;
- (void)stop;
@end

@implementation HCHereChrome
- (id)init {
    if (!(self=[super init])) return nil;
    text=[@"" retain];status=[@"Starting Chrome…" retain];input=[[NSMutableString alloc] init];dirty=YES;
    chrome=[[HCChromePage alloc] initWithScript:[NSString stringWithUTF8String:(const char *)browser_script]
        changed:^(NSDictionary *event) {
            if (stopped) return;
            NSString *type=event[@"type"];
            if ([type isEqual:@"error"]) { hhh_string_set(&status,hhh_chrome_string(event[@"message"]));dirty=YES; }
            else {
                if ([type isEqual:@"navigation"]) {
                    prompt=0;[input setString:@""];offset=0;
                    hhh_string_set(&status,@"Loading page…");
                }
                [self project];
            }
        }];
    return self;
}
- (void)project {
    if (stopped || !chrome->ready) return;
    if (requesting || prompt) { again=YES;return; }
    requesting=YES;
    [chrome evaluate:@"globalThis.hardcoreBrowser?.project()" completion:^(id value,NSError *error) {
        requesting=NO;
        if (stopped) return;
        if (!error && [value isKindOfClass:NSDictionary.class] && [value[@"blocks"] isKindOfClass:NSArray.class]) {
            [snapshot release];snapshot=[value retain];revision=[value[@"generation"] intValue];
            NSMutableString *output=[NSMutableString string];NSMutableArray *targets=[NSMutableArray array];
            int count=0;
            for (id raw in value[@"blocks"]) {
                if (++count>2000 || output.length>=HHH_TEXT_MAX) break;
                if (![raw isKindOfClass:NSDictionary.class]) continue;
                NSDictionary *block=raw;
                NSArray *columns=[block[@"columns"] isKindOfClass:NSArray.class] ? block[@"columns"] : nil;
                NSArray *groups=columns ?: @[[block[@"runs"] isKindOfClass:NSArray.class] ? block[@"runs"] : @[]];
                int column=0,lastAction=0,indent=MIN(16,MAX(0,[block[@"indent"] intValue]));
                if (output.length) hhh_append(output,@"\n");
                if ([block[@"gap"] intValue]>0) hhh_append(output,@"\n");
                for (int i=0;i<indent;i++) hhh_append(output,@" ");
                for (id group in groups) {
                    if (column++>=16) break;
                    if (column>1) hhh_append(output,@" │ ");
                    if (![group isKindOfClass:NSArray.class]) continue;
                    for (id rawRun in group) {
                        if (![rawRun isKindOfClass:NSDictionary.class]) continue;
                        int action=[rawRun[@"action"] intValue];
                        if (action<1 || action>4000) action=0;
                        if (action && action!=lastAction && targets.count<4000) {
                            NSNumber *key=@(action);NSUInteger index=[targets indexOfObject:key];
                            if (index==NSNotFound) {index=targets.count;[targets addObject:key];}
                            hhh_append(output,[NSString stringWithFormat:@"[%lu] ",(unsigned long)index+1]);
                        }
                        lastAction=action;
                        NSDictionary *image=[rawRun[@"image"] isKindOfClass:NSDictionary.class] ? rawRun[@"image"] : nil;
                        if (image) hhh_append(output,[NSString stringWithFormat:@"[%@]",hhh_chrome_string(image[@"alt"])]);
                        else hhh_append(output,hhh_chrome_string(rawRun[@"text"]));
                    }
                }
            }
            [actions release];actions=[targets copy];hhh_string_set(&text,output);
            hhh_string_set(&address,hhh_chrome_string(value[@"url"]));
            hhh_string_set(&status,[NSString stringWithFormat:@"Chrome · %lu links and controls",(unsigned long)actions.count]);
            cols=0;dirty=YES;
        } else if (error && !text.length) {hhh_string_set(&status,error.localizedDescription);dirty=YES;}
        if (again && !prompt) {again=NO;[self project];}
    }];
}
- (void)applyControl:(id)value {
    if (stopped) return;
    NSArray *args=value ? @[@(controlNode),@(controlRevision),value] : @[@(controlNode),@(controlRevision)];
    NSData *json=[NSJSONSerialization dataWithJSONObject:args options:0 error:NULL];
    NSString *encoded=[[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] autorelease];
    NSString *code=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.activate(...%@)",encoded];
    prompt=0;[input setString:@""];
    [chrome evaluate:code completion:^(id result,NSError *error) {
        if (stopped) return;
        if (error || ![result boolValue]) hhh_string_set(&status,@"Page changed; choose the control again.");
        [self project];dirty=YES;
    }];
}
- (void)choose:(int)number {
    if (number<1 || number>(int)actions.count) {hhh_string_set(&status,@"Choose a displayed link or control number.");prompt=0;dirty=YES;return;}
    controlNode=[actions[number-1] intValue];controlRevision=revision;prompt=5;
    NSString *code=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.info(%d,%d)",controlNode,controlRevision];
    [chrome evaluate:code completion:^(id result,NSError *error) {
        if (stopped) return;
        prompt=0;dirty=YES;
        if (error || ![result isKindOfClass:NSDictionary.class]) {
            hhh_string_set(&status,@"Page changed; choose the control again.");[self project];return;
        }
        [control release];control=[result retain];
        NSString *tag=result[@"tag"],*type=result[@"type"];
        if ([tag isEqual:@"SELECT"]) {
            prompt=4;[input setString:@""];
            NSMutableString *choices=[NSMutableString string];int n=0;
            for (NSDictionary *option in result[@"options"]) {
                if (++n>100) break;
                hhh_append(choices,[NSString stringWithFormat:@"%d %@  ",n,hhh_chrome_string(option[@"label"])]);
            }
            hhh_string_set(&status,choices);
        } else if ([tag isEqual:@"TEXTAREA"] || ([tag isEqual:@"INPUT"] &&
            ![@[@"checkbox",@"radio",@"button",@"submit",@"reset",@"file"] containsObject:type])) {
            prompt=3;[input setString:hhh_chrome_string(result[@"value"])];
            hhh_string_set(&status,[type isEqual:@"password"] ? @"Password (hidden) · Enter applies · Esc cancels" : @"Enter applies · Ctrl-U clears · Esc cancels");
        } else if ([type isEqual:@"file"]) {
            hhh_string_set(&status,@"File upload requires Chrome’s page view.");
        } else [self applyControl:nil];
    }];
}
- (void)stop {
    stopped=YES;__block BOOL completed=NO;
    [chrome closeWithCompletion:^(NSError *error){(void)error;completed=YES;}];
    NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+1;
    while (!completed && NSDate.timeIntervalSinceReferenceDate<deadline)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
- (void)dealloc {
    [chrome release];[snapshot release];[control release];[actions release];[text release];[status release];[address release];[input release];[super dealloc];
}
@end

static int hc_here_web_main(int argc,char **argv) {
    @autoreleasepool {
        NSString *url=hhh_chrome_url(argc>1 ? [NSString stringWithUTF8String:argv[1]] : @"https://www.wikipedia.org/");
        if (!url) {fputs("Web: enter an HTTP or HTTPS URL without embedded credentials.\n",stderr);return 2;}
        HHHTerm t;if (!hhh_start(&t)) return 2;
        HCHereChrome *b=[[HCHereChrome alloc] init];HHHLines lines={0};
        hhh_string_set(&b->address,url);
        [b->chrome start:url completion:^(NSError *error) {
            if (b->stopped) return;
            if (error) hhh_string_set(&b->status,error.localizedDescription);
            else [b project];
            b->dirty=YES;
        }];
        while (!hhh_stop) {
          @autoreleasepool {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
            hhh_size(&t);int bodyrows=t.rows-4;
            if (b->cols!=t.cols) {
                hhh_wrap(&lines,b->text.UTF8String,t.cols);b->cols=t.cols;b->dirty=YES;
            }
            if (b->viewportCols!=t.cols || b->viewportRows!=t.rows) {
                b->viewportCols=t.cols;b->viewportRows=t.rows;
                [b->chrome setWidth:t.cols*8 height:MAX(200,bodyrows*16)];
            }
            b->offset=MAX(0,MIN(b->offset,lines.count-bodyrows));
            if (b->dirty || hhh_resize) {
                NSString *title=hhh_chrome_string(b->snapshot[@"title"]);
                hhh_line(1,t.cols,"\033[1;7m",[[NSString stringWithFormat:@"Chrome · %@",title.length ? title : @"Web"] UTF8String]);
                hhh_line(2,t.cols,"\033[0;36m",b->address.UTF8String ?: "");
                for (int r=0;r<bodyrows;r++) hhh_line(r+3,t.cols,"\033[0m",b->offset+r<lines.count ? lines.line[b->offset+r] : "");
                hhh_line(t.rows-1,t.cols,"\033[0;2m",b->status.UTF8String);
                NSString *bottom=@"j/k scroll | Space page | number↵ link/control | b/f back/next | u URL | r reload | q close";
                if (b->prompt && b->prompt!=5) {
                    NSString *value=b->input;
                    if (b->prompt==3 && [b->control[@"type"] isEqual:@"password"]) value=[@"" stringByPaddingToLength:MIN(200,value.length) withString:@"•" startingAtIndex:0];
                    NSString *label=b->prompt==1 ? @"URL" : b->prompt==2 ? @"Link/control" : b->prompt==4 ? @"Option number" : hhh_chrome_string(b->control[@"label"]);
                    bottom=[NSString stringWithFormat:@"%@: %@",label,value];
                }
                hhh_line(t.rows,t.cols,"\033[0;7m",bottom.UTF8String);fputs("\033[0m",stdout);fflush(stdout);b->dirty=hhh_resize=0;
            }
            int k=hhh_key(&t,b->prompt==1 || b->prompt==3);if (!k) continue;b->dirty=YES;
            if (k==3) break;
            if (b->prompt) {
                if (k==27) {b->prompt=0;[b->input setString:@""];[b project];}
                else if (b->prompt==5) continue;
                else if (k==127 || k==8) {if (b->input.length) [b->input deleteCharactersInRange:[b->input rangeOfComposedCharacterSequenceAtIndex:b->input.length-1]];}
                else if (k==21) [b->input setString:@""];
                else if (k=='\r' || k=='\n') {
                    int prompt=b->prompt;b->prompt=0;
                    if (prompt==1) {
                        NSString *target=hhh_chrome_url(b->input);
                        if (target) {[b->chrome navigate:target];b->offset=0;}
                        else hhh_string_set(&b->status,@"Enter a valid HTTP(S) URL.");
                    } else if (prompt==2) [b choose:b->input.intValue];
                    else if (prompt==3) [b applyControl:[[b->input copy] autorelease]];
                    else if (prompt==4) {
                        int choice=b->input.intValue;NSArray *options=b->control[@"options"];
                        if (choice>=1 && choice<=(int)options.count) [b applyControl:options[choice-1][@"value"] ?: @""];
                        else hhh_string_set(&b->status,@"Choose one of the option numbers.");
                    }
                    [b->input setString:@""];
                } else if (k<0x110000 && hhh_printable(k) && b->input.length<HHH_URL_MAX) {
                    char utf[5]={0};utf8_encode(k,utf);[b->input appendString:[NSString stringWithUTF8String:utf] ?: @""];
                }
                continue;
            }
            if (k=='q' || k==27) break;
            if (k=='j' || k==HHH_DOWN) b->offset++;
            if (k=='k' || k==HHH_UP) b->offset--;
            if (k==' ' || k==HHH_PGDN) b->offset+=bodyrows;
            if (k==HHH_PGUP) b->offset-=bodyrows;
            if (k=='g' || k==HHH_HOME) b->offset=0;
            if (k=='G' || k==HHH_END) b->offset=lines.count;
            if (k=='u') {b->prompt=1;[b->input setString:@""];}
            if (k>='0' && k<='9') {b->prompt=2;[b->input setString:[NSString stringWithFormat:@"%c",k]];}
            if (k=='r') [b->chrome reload];
            if (k=='b') [b->chrome go:-1];
            if (k=='f') [b->chrome go:1];
          }
        }
        [b stop];[b release];hhh_lines_free(&lines);hhh_finish(&t);return 0;
    }
}
#endif
