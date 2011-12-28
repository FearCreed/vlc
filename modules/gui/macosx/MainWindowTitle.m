/*****************************************************************************
 * MainWindowTitle.m: MacOS X interface module
 *****************************************************************************
 * Copyright (C) 2011 Felix Paul Kühne
 * $Id$
 *
 * Authors: Felix Paul Kühne <fkuehne -at- videolan -dot- org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#import <vlc_common.h>
#import "intf.h"
#import "MainWindowTitle.h"
#import "CoreInteraction.h"

/*****************************************************************************
 * VLCMainWindowTitleView
 *****************************************************************************/

@implementation VLCMainWindowTitleView

- (void)awakeFromNib
{
    [self setImageScaling: NSScaleToFit];
    [self setImageFrameStyle: NSImageFrameNone];
    [self setImageAlignment: NSImageAlignCenter];
    [self setImage: [NSImage imageNamed:@"bottom-background_dark"]];
    [self setAutoresizesSubviews: YES];

    [o_red_btn setImage: [NSImage imageNamed:@"window-close"]];
    [o_red_btn setAlternateImage: [NSImage imageNamed:@"window-close-on"]];
    [[o_red_btn cell] setShowsBorderOnlyWhileMouseInside: YES];
    [o_yellow_btn setImage: [NSImage imageNamed:@"window-minimize"]];
    [o_yellow_btn setAlternateImage: [NSImage imageNamed:@"window-minimize-on"]];
    [[o_yellow_btn cell] setShowsBorderOnlyWhileMouseInside: YES];
    [o_green_btn setImage: [NSImage imageNamed:@"window-zoom"]];
    [o_green_btn setAlternateImage: [NSImage imageNamed:@"window-zoom-on"]];
    [[o_green_btn cell] setShowsBorderOnlyWhileMouseInside: YES];
}

- (BOOL)mouseDownCanMoveWindow
{
    return YES;
}

- (IBAction)buttonAction:(id)sender
{
    if (sender == o_red_btn)
        [[self window] orderOut: sender];
    else if (sender == o_yellow_btn)
        [[self window] miniaturize: sender];
    else if (sender == o_green_btn)
        [[self window] performZoom: sender];
    else if (sender == o_fullscreen_btn)
        [[VLCCoreInteraction sharedInstance] toggleFullscreen];
    else
        msg_Err( VLCIntf, "unknown button action sender" );
}

- (void)setWindowTitle:(NSString *)title
{
    [o_title_lbl setStringValue: title];
}

- (void)setFullscreenButtonHidden:(BOOL)b_value
{
    [o_fullscreen_btn setHidden: b_value];
}

- (void)setWindowButtonOver:(BOOL)b_value
{
    if( b_value )
    {
        [o_red_btn setImage: [NSImage imageNamed:@"window-close-over"]];
        [o_yellow_btn setImage: [NSImage imageNamed:@"window-minimize-over"]];
        [o_green_btn setImage: [NSImage imageNamed:@"window-zoom-over"]];
    }
    else
    {
        [o_red_btn setImage: [NSImage imageNamed:@"window-close"]];
        [o_yellow_btn setImage: [NSImage imageNamed:@"window-minimize"]];
        [o_green_btn setImage: [NSImage imageNamed:@"window-zoom"]];
    }
}

@end

@implementation VLCWindowButtonCell

- (void)mouseEntered:(NSEvent *)theEvent
{
    [(VLCMainWindowTitleView *)[[self controlView] superview] setWindowButtonOver: YES];
}

- (void)mouseExited:(NSEvent *)theEvent
{
    [(VLCMainWindowTitleView *)[[self controlView] superview] setWindowButtonOver: NO];
}

@end
