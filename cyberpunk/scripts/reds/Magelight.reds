// Magelight's natives (MagelightCP.dll). A view is named by its mod id (its folder under r6/magelight) and its name
// in the manifest. Every call returns at once; what it changes shows within a frame.

public native func Magelight_IsReady() -> Bool;                                    // the host made its views
public native func Magelight_HasView(mod: String, view: String) -> Bool;
public native func Magelight_Show(mod: String, view: String, show: Bool) -> Bool;  // hiding the UI-mode view closes it
public native func Magelight_IsVisible(mod: String, view: String) -> Bool;
// To the page: window[channel](text) if the page defined it, else magelight.on(channel), held until one subscribes.
public native func Magelight_Send(mod: String, view: String, channel: String, text: String) -> Bool;
public native func Magelight_OpenUIMode(mod: String, view: String) -> Bool;        // false for a HUD view
public native func Magelight_CloseUIMode() -> Void;
public native func Magelight_UIModeView() -> String;                               // "mod/view", "" = none
public native func Magelight_Reload(mod: String, view: String) -> Bool;
// The CET door's queue (scripts/cet/magelight); redscript listens through MagelightEvents.reds instead.
public native func Magelight_NextMessage() -> String;
