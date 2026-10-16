/* Source metrics are backing pixels; NSView frames are parent-space points.
   Remove the top edge once, accounting for the actual parent's orientation. */
#ifndef HARDCORE_RECORDING_VIEW_GEOMETRY_H
#define HARDCORE_RECORDING_VIEW_GEOMETRY_H
static NSRect hc_recording_content_rect(NSRect bounds,CGFloat scale,int titlePixels,int dockPixels,int cellPixels,BOOL rightDock,BOOL flipped) {
    if(!isfinite(scale) || scale<=0)scale=1;
    CGFloat top=MIN(MAX(0,titlePixels/scale),MAX(0,bounds.size.height-1));
    CGFloat side=MIN(MAX(0,(dockPixels+2.0*cellPixels)/scale),MAX(0,bounds.size.width-1));
    bounds.size.height-=top;bounds.size.width-=side;
    if(flipped)bounds.origin.y+=top;
    if(!rightDock)bounds.origin.x+=side;
    return bounds;
}
#endif
