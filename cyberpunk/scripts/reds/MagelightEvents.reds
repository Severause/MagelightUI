// What pages send, on Codeware's CallbackSystem. Needs Codeware: without it this file does not compile and
// Magelight.reds' natives still work.
//
//   GameInstance.GetCallbackSystem().RegisterCallback(n"Magelight/MyMod/buy", this, n"OnBuy");
//   private cb func OnBuy(evt: ref<MagelightEvent>) { ... evt.payload ... }
//
// The host's own: Magelight/<ModId>/__ready (a page loaded) and Magelight/<ModId>/__uimode (payload "1" / "0").
// Never while a game loads: those wait until it is over.

public class MagelightEvent extends CallbackSystemEvent {
    public let mod: String;
    public let view: String;
    public let channel: String;
    public let payload: String;   // what the page sent; an object arrives as JSON text
}

// Called by MagelightCP.dll on the game thread, once per message. false = no CallbackSystem.
public static func Magelight_Fire(name: String, mod: String, view: String, channel: String, payload: String) -> Bool {
    let cbs = GameInstance.GetCallbackSystem();
    if !IsDefined(cbs) {
        return false;
    }
    let event = StringToName(name);
    cbs.RegisterEvent(event, n"MagelightEvent");
    let evt = new MagelightEvent();
    evt.mod = mod;
    evt.view = view;
    evt.channel = channel;
    evt.payload = payload;
    cbs.DispatchEventAs(event, evt);
    return true;
}
