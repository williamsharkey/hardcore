/* Chrome paints in its own process. This NSView displays captured frames and
   forwards native input through CDP; Hardcore's Metal/grid chrome stays above. */
#ifndef HARDCORE_BROWSER_CHROME_VIEW_H
#define HARDCORE_BROWSER_CHROME_VIEW_H
@interface HCChromeBrowserView : NSView <NSTextInputClient> {
    HCChromePage *page;
    NSImage *frameImage;
    NSTimer *captureTimer;
    BOOL rendering,capturing,pendingCapture;
    NSInteger buttons;
    NSUInteger pointerSerial;
    NSMutableAttributedString *marked;
    NSTrackingArea *pointerTracking;
}
- (id)initWithFrame:(NSRect)rect page:(HCChromePage *)value;
- (void)setRendering:(BOOL)value;
- (void)refresh;
- (void)clearFrame;
@end
static int hc_chrome_modifiers(NSEventModifierFlags flags) {
    return ((flags&NSEventModifierFlagOption)?1:0) | ((flags&NSEventModifierFlagControl)?2:0) |
        ((flags&NSEventModifierFlagCommand)?4:0) | ((flags&NSEventModifierFlagShift)?8:0);
}
static NSDictionary *hc_chrome_key(NSEvent *event) {
    NSString *key=nil,*code=nil;int vk=0;
    switch(event.keyCode) {
    case 36:case 76:key=@"Enter";code=@"Enter";vk=13;break;
    case 48:key=@"Tab";code=@"Tab";vk=9;break;
    case 51:key=@"Backspace";code=@"Backspace";vk=8;break;
    case 117:key=@"Delete";code=@"Delete";vk=46;break;
    case 53:key=@"Escape";code=@"Escape";vk=27;break;
    case 123:key=@"ArrowLeft";code=@"ArrowLeft";vk=37;break;
    case 124:key=@"ArrowRight";code=@"ArrowRight";vk=39;break;
    case 125:key=@"ArrowDown";code=@"ArrowDown";vk=40;break;
    case 126:key=@"ArrowUp";code=@"ArrowUp";vk=38;break;
    case 115:key=@"Home";code=@"Home";vk=36;break;
    case 119:key=@"End";code=@"End";vk=35;break;
    case 116:key=@"PageUp";code=@"PageUp";vk=33;break;
    case 121:key=@"PageDown";code=@"PageDown";vk=34;break;
    default:break;
    }
    if(!key) {
        key=event.characters ?: @"";
        NSString *base=event.charactersIgnoringModifiers.uppercaseString;
        if(base.length==1){unichar ch=[base characterAtIndex:0];vk=ch;
            if(ch>='A' && ch<='Z')code=[@"Key" stringByAppendingString:base];
            else if(ch>='0' && ch<='9')code=[@"Digit" stringByAppendingString:base];
            else if(ch==' ')code=@"Space";}
    }
    return @{@"key":key?:@"",@"code":code?:@"",@"windowsVirtualKeyCode":@(vk),@"nativeVirtualKeyCode":@(event.keyCode),
        @"modifiers":@(hc_chrome_modifiers(event.modifierFlags))};
}
@implementation HCChromeBrowserView
- (id)initWithFrame:(NSRect)rect page:(HCChromePage *)value {
    if((self=[super initWithFrame:rect])) {page=[value retain];marked=[[NSMutableAttributedString alloc] init];}
    return self;
}
- (void)dealloc {
    [captureTimer invalidate];[captureTimer release];[frameImage release];[marked release];[pointerTracking release];[page release];[super dealloc];
}
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event;return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (void)updateTrackingAreas {
    if(pointerTracking){[self removeTrackingArea:pointerTracking];[pointerTracking release];}
    pointerTracking=[[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved|NSTrackingActiveInKeyWindow|NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:pointerTracking];[super updateTrackingAreas];
}
- (void)setRendering:(BOOL)value {
    rendering=value;
    if(value && !captureTimer) {
        captureTimer=[[NSTimer scheduledTimerWithTimeInterval:0.12 target:self selector:@selector(refresh) userInfo:nil repeats:YES] retain];
        [[NSRunLoop mainRunLoop] addTimer:captureTimer forMode:NSRunLoopCommonModes];
        [self refresh];
    } else if(!value && captureTimer) {[captureTimer invalidate];[captureTimer release];captureTimer=nil;}
}
- (void)clearFrame { [frameImage release];frameImage=nil;[self setNeedsDisplay:YES]; }
- (void)refresh {
    if(!rendering || self.hidden || !page->ready || page->closed)return;
    if(capturing){pendingCapture=YES;return;}
    capturing=YES;
    [page capture:^(NSData *data,NSError *error){
        capturing=NO;
        if(!error && data.length && rendering) {
            NSImage *next=[[[NSImage alloc] initWithData:data] autorelease];
            if(next){[frameImage release];frameImage=[next retain];[self setNeedsDisplay:YES];}
        }
        if(pendingCapture){pendingCapture=NO;[self refresh];}
    }];
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;[[NSColor colorWithCalibratedWhite:0.08 alpha:1] setFill];NSRectFill(self.bounds);
    if(frameImage) {
        [NSGraphicsContext currentContext].imageInterpolation=NSImageInterpolationHigh;
        [frameImage drawInRect:self.bounds fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:YES hints:nil];
    } else {
        NSString *label=page->ready?@"Rendering Chrome…":@"Connecting to Chrome…";
        [label drawAtPoint:NSMakePoint(14,50) withAttributes:@{NSForegroundColorAttributeName:NSColor.lightGrayColor,NSFontAttributeName:[NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular]}];
    }
}
- (void)mouse:(NSEvent *)event type:(NSString *)type button:(NSString *)button mask:(NSInteger)mask {
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];
    if([type isEqual:@"mousePressed"]){buttons|=mask;[self.window makeFirstResponder:self];}
    if([type isEqual:@"mouseReleased"])buttons&=~mask;
    [page command:@"Input.dispatchMouseEvent" params:@{@"type":type,@"x":@(point.x),@"y":@(point.y),@"button":button,
        @"buttons":@(buttons),@"clickCount":@(MAX(0,event.clickCount)),@"modifiers":@(hc_chrome_modifiers(event.modifierFlags))}
        completion:^(NSDictionary *result,NSError *error){(void)result;(void)error;[self refresh];}];
}
- (void)mouseDown:(NSEvent *)e { [self mouse:e type:@"mousePressed" button:@"left" mask:1]; }
- (void)mouseUp:(NSEvent *)e { [self mouse:e type:@"mouseReleased" button:@"left" mask:1]; }
- (void)mouseDragged:(NSEvent *)e { [self mouse:e type:@"mouseMoved" button:@"left" mask:1]; }
- (void)mouseMoved:(NSEvent *)e {
    [self mouse:e type:@"mouseMoved" button:@"none" mask:0];
    NSPoint point=[self convertPoint:e.locationInWindow fromView:nil];NSUInteger serial=++pointerSerial;
    NSString *code=[NSString stringWithFormat:@"(()=>{const e=document.elementFromPoint(%f,%f);return e?getComputedStyle(e).cursor:'auto'})()",point.x,point.y];
    [page evaluate:code completion:^(id value,NSError *error){
        if(error || serial!=pointerSerial || !rendering || self.hidden)return;
        NSPoint current=[self.window.contentView convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
        if([self.window.contentView hitTest:current]!=self)return;
        NSCursor *cursor=[value isEqual:@"pointer"]?NSCursor.pointingHandCursor:[value isEqual:@"text"]?NSCursor.IBeamCursor:
            [value isEqual:@"crosshair"]?NSCursor.crosshairCursor:[value isEqual:@"grab"]?NSCursor.openHandCursor:
            [value isEqual:@"grabbing"]?NSCursor.closedHandCursor:NSCursor.arrowCursor;
        [cursor set];
    }];
}
- (void)rightMouseDown:(NSEvent *)e { [self mouse:e type:@"mousePressed" button:@"right" mask:2]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouse:e type:@"mouseReleased" button:@"right" mask:2]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouse:e type:@"mouseMoved" button:@"right" mask:2]; }
- (void)otherMouseDown:(NSEvent *)e { [self mouse:e type:@"mousePressed" button:@"middle" mask:4]; }
- (void)otherMouseUp:(NSEvent *)e { [self mouse:e type:@"mouseReleased" button:@"middle" mask:4]; }
- (void)otherMouseDragged:(NSEvent *)e { [self mouse:e type:@"mouseMoved" button:@"middle" mask:4]; }
- (void)scrollWheel:(NSEvent *)event {
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];
    double step=event.hasPreciseScrollingDeltas?1:32;
    [page command:@"Input.dispatchMouseEvent" params:@{@"type":@"mouseWheel",@"x":@(point.x),@"y":@(point.y),
        @"deltaX":@(-event.scrollingDeltaX*step),@"deltaY":@(-event.scrollingDeltaY*step),@"modifiers":@(hc_chrome_modifiers(event.modifierFlags))}
        completion:^(NSDictionary *result,NSError *error){(void)result;(void)error;[self refresh];}];
}
- (void)dispatchKey:(NSEvent *)event type:(NSString *)type {
    NSMutableDictionary *params=[NSMutableDictionary dictionaryWithDictionary:hc_chrome_key(event)];params[@"type"]=type;
    [page command:@"Input.dispatchKeyEvent" params:params completion:^(NSDictionary *result,NSError *error){(void)result;(void)error;[self refresh];}];
}
- (void)keyDown:(NSEvent *)event {
    if(event.modifierFlags&NSEventModifierFlagCommand){[self dispatchKey:event type:@"rawKeyDown"];return;}
    if(event.modifierFlags&NSEventModifierFlagControl){[self dispatchKey:event type:@"rawKeyDown"];return;}
    NSString *key=hc_chrome_key(event)[@"key"];
    if([@[@"Enter",@"Tab",@"Backspace",@"Delete",@"Escape",@"ArrowLeft",@"ArrowRight",@"ArrowUp",@"ArrowDown",@"Home",@"End",@"PageUp",@"PageDown"] containsObject:key]) {
        [self dispatchKey:event type:@"rawKeyDown"];
        if([key isEqual:@"Enter"])[page command:@"Input.dispatchKeyEvent" params:@{@"type":@"char",@"text":@"\r",@"unmodifiedText":@"\r",@"windowsVirtualKeyCode":@13} completion:nil];
        return;
    }
    [self dispatchKey:event type:@"rawKeyDown"];
    [self interpretKeyEvents:@[event]];
}
- (void)keyUp:(NSEvent *)event { [self dispatchKey:event type:@"keyUp"]; }
- (void)insertText:(id)value replacementRange:(NSRange)range {
    (void)range;NSString *text=[value isKindOfClass:NSAttributedString.class]?[value string]:value;
    [marked setAttributedString:[[[NSAttributedString alloc] initWithString:@""] autorelease]];
    if(text.length)[page command:@"Input.insertText" params:@{@"text":text} completion:^(NSDictionary *result,NSError *error){(void)result;(void)error;[self refresh];}];
}
- (void)insertText:(id)value { [self insertText:value replacementRange:NSMakeRange(NSNotFound,0)]; }
- (void)doCommandBySelector:(SEL)selector { (void)selector;NSEvent *event=NSApp.currentEvent;if(event.type==NSEventTypeKeyDown)[self dispatchKey:event type:@"rawKeyDown"]; }
- (void)copy:(id)sender {
    (void)sender;[page evaluate:@"(()=>{const e=document.activeElement;return e && /^(INPUT|TEXTAREA)$/.test(e.tagName) && e.type!=='password' ? e.value.slice(e.selectionStart||0,e.selectionEnd||0) : globalThis.getSelection()?.toString() || ''})()"
        completion:^(id value,NSError *error){if(!error && [value isKindOfClass:NSString.class]){NSPasteboard *p=NSPasteboard.generalPasteboard;[p clearContents];[p setString:value forType:NSPasteboardTypeString];}}];
}
- (void)paste:(id)sender { (void)sender;NSString *text=[NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];if(text)[self insertText:text]; }
- (void)cut:(id)sender {
    [self copy:sender];[page command:@"Input.dispatchKeyEvent" params:@{@"type":@"rawKeyDown",@"key":@"Backspace",@"windowsVirtualKeyCode":@8} completion:nil];
    [page command:@"Input.dispatchKeyEvent" params:@{@"type":@"keyUp",@"key":@"Backspace",@"windowsVirtualKeyCode":@8} completion:nil];
}
- (void)selectAll:(id)sender { (void)sender;[page evaluate:@"(()=>{const e=document.activeElement;if(e && /^(INPUT|TEXTAREA)$/.test(e.tagName)){e.select();return;}const s=getSelection();s.removeAllRanges();const r=document.createRange();r.selectNodeContents(document.body);s.addRange(r);})()" completion:nil];[self refresh]; }
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if(!(event.modifierFlags&NSEventModifierFlagCommand))return [super performKeyEquivalent:event];
    NSString *key=event.charactersIgnoringModifiers.lowercaseString;
    if([key isEqual:@"c"]){[self copy:nil];return YES;}if([key isEqual:@"v"]){[self paste:nil];return YES;}
    if([key isEqual:@"x"]){[self cut:nil];return YES;}if([key isEqual:@"a"]){[self selectAll:nil];return YES;}
    return [super performKeyEquivalent:event];
}
- (BOOL)hasMarkedText { return marked.length>0; }
- (NSRange)markedRange { return marked.length?NSMakeRange(0,marked.length):NSMakeRange(NSNotFound,0); }
- (NSRange)selectedRange { return NSMakeRange(0,0); }
- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement {
    (void)replacement;NSAttributedString *text=[value isKindOfClass:NSAttributedString.class]?value:[[[NSAttributedString alloc] initWithString:value] autorelease];[marked setAttributedString:text];
    [page command:@"Input.imeSetComposition" params:@{@"text":marked.string,@"selectionStart":@(selection.location),@"selectionEnd":@(NSMaxRange(selection))} completion:nil];
}
- (void)unmarkText { if(marked.length){NSString *text=[[marked.string copy] autorelease];[self insertText:text];} }
- (NSArray *)validAttributesForMarkedText { return @[]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual { (void)range;if(actual)*actual=NSMakeRange(NSNotFound,0);return nil; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point;return 0; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    if(actual)*actual=range;return [self.window convertRectToScreen:[self convertRect:NSMakeRect(0,0,1,20) toView:nil]];
}
@end
#endif
