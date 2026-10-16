#import <Foundation/Foundation.h>
#include <math.h>
#include "../hardcore_recording_view_geometry.h"
int main(void) {
    NSRect bounds=NSMakeRect(0,0,1000,700);
    NSRect left=hc_recording_content_rect(bounds,2,96,560,20,NO,NO);
    if(left.origin.x!=300 || left.origin.y!=0 || left.size.width!=700 || left.size.height!=652)return 1;
    if(NSMaxY(left)!=NSMaxY(bounds)-48)return 2; /* unflipped parent: top is maxY */
    NSRect right=hc_recording_content_rect(bounds,2,96,560,20,YES,NO);
    if(right.origin.x!=0 || NSMaxX(right)!=700 || NSMaxY(right)!=652)return 3;
    NSRect flipped=hc_recording_content_rect(bounds,2,96,560,20,NO,YES);
    if(flipped.origin.y!=48 || NSMaxY(flipped)!=700 || flipped.origin.x!=left.origin.x)return 4;
    NSRect one=hc_recording_content_rect(bounds,1,48,280,10,YES,NO);
    if(!NSEqualRects(one,right))return 5; /* same physical layout on 1x / 2x */
    NSRect tiny=hc_recording_content_rect(NSMakeRect(10,20,15,12),2,96,560,20,NO,YES);
    if(tiny.size.width!=1 || tiny.size.height!=1 || !NSContainsRect(NSMakeRect(10,20,15,12),tiny))return 6;
    NSRect invalid=hc_recording_content_rect(bounds,NAN,-1,-1,0,YES,NO);
    if(!NSEqualRects(invalid,bounds))return 7;
    puts("PASS Clips view geometry: title exclusion, left/right dock, point/pixel scaling, both parent orientations, small bounds");return 0;
}
