// The demo mod's script half (needs Codeware): answers its panel's page, so one look shows the redscript path.
public class MagelightDemoScript extends ScriptableService {
    private let clicks: Int32;

    private cb func OnLoad() {
        let cbs = GameInstance.GetCallbackSystem();
        cbs.RegisterCallback(n"Magelight/MagelightDemo/__ready", this, n"OnReady");
        cbs.RegisterCallback(n"Magelight/MagelightDemo/demo.click", this, n"OnClick");
    }

    private cb func OnReady(evt: ref<MagelightEvent>) {
        if Equals(evt.view, "panel") {
            Magelight_Send("MagelightDemo", "panel", "demo.echo", "redscript heard the page load");
        }
    }

    private cb func OnClick(evt: ref<MagelightEvent>) {
        this.clicks += 1;
        Magelight_Send("MagelightDemo", "panel", "demo.echo", "redscript heard click " + ToString(this.clicks));
    }
}
