# interface/

`magelightfocus.swf` — the blank Scaleform movie behind Magelight's own engine
menu (`MagelightFocus`, see `RegisterFocusMenu` in `src/Magelight.cpp`). The
menu exists for its engine semantics — cursor ownership, menu input context,
Cancel events, optional pause — and draws nothing; the movie is the 25-byte
blank SWF the SeverActions carrier menu also uses. Staged to `Data/Interface/`.
