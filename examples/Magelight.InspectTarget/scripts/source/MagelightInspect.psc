ScriptName MagelightInspect extends Quest
{ Magelight.InspectTarget — the Papyrus side.
  Owns the trigger (a hotkey + the crosshair), finds the C++-created view
  through the Magelight Papyrus tier, and asks the plugin to open it. The
  heavy lifting — the live actor snapshot — is the C++ native Open() below.
  Attach this to a start-game-enabled quest. }

; The C++ plugin (plugin.cpp) registers this native: it gathers the actor's
; live stats, pushes them to the "inspect" view, shows it and takes UI mode.
; False = the view is not ready or another mod holds UI mode.
bool Function Open(Actor akActor) Global Native

Int Property InspectKey = 24 Auto   ; DXScanCode 24 = 'O'; change it here (the example ships no MCM)

Event OnInit()
    ; RegisterForKey registrations PERSIST across save/load, so registering
    ; once when the quest first starts is enough — a Quest script never
    ; receives OnPlayerLoadGame (that event fires only on Actor/alias scripts),
    ; so do NOT try to re-register there. If you add this to an existing save,
    ; the quest's first start fires OnInit.
    RegisterForKey(InspectKey)
EndEvent

Event OnKeyDown(Int keyCode)
    If keyCode != InspectKey || Utility.IsInMenuMode()
        Return
    EndIf

    Actor target = Game.GetCurrentCrosshairRef() as Actor
    If !target || target == Game.GetPlayer()
        Debug.Notification("Inspect: look at an NPC")
        Return
    EndIf

    ; The Magelight Papyrus tier can read any mod: find the view the C++
    ; plugin created, by the shared modId. It cannot show or focus that view
    ; (host 0.30.0: the mod is plugin-owned), so the plugin's own native does.
    If Magelight.FindView("MagelightInspect", "inspect") == 0
        Debug.Notification("Inspect: Magelight view not ready")
        Return
    EndIf

    ; Hand the actor to C++: it gathers the live stats, fills the page and
    ; opens it (cursor + suspended controls, game not paused).
    If !Open(target)
        Debug.Notification("Inspect: " + Magelight.GetLastError("MagelightInspect"))
    EndIf
EndEvent
