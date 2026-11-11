using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;
using PRoCon.Core;
using PRoCon.Core.Plugin;
using PRoCon.Core.Remote;

// Puts ProCon's own Server Settings > Gameplay page for MOHW (from PRoCon.exe) in a window, connected through the real PRoCon.Core
// to the test server, and works its boxes the way a click does. Screenshots go next to this file.
class Harness3
{
    static PRoConApplication app;
    static PRoConClient c;
    static Control page;
    static Form form;
    static string outDir;

    static void P(string s) { Console.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " " + s); Console.Out.Flush(); }

    static object Field(string name)
    {
        for (Type t = page.GetType(); t != null; t = t.BaseType)
        {
            FieldInfo f = t.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (f != null) return f.GetValue(page);
        }
        throw new Exception("no field " + name);
    }

    static CheckBox Box(string name) { return (CheckBox)Field(name); }

    static string State()
    {
        string s = "";
        foreach (string n in new[] { "chkSettingsHardcore", "chkSettingsFriendlyFire", "chkSettingsKillCam", "chkSettingsRegenerateHealth", "chkSettingsHudMinimap", "chkSettingsTeamBalance" })
        {
            CheckBox b = Box(n);
            s += n.Replace("chkSettings", "") + "=" + (b.Checked ? "on" : "off") + (b.Enabled ? "" : "(greyed)") + " ";
        }
        PluginDetails d = c.PluginsManager.GetPluginDetails("CMohwBots");
        foreach (CPluginVariable v in d.DisplayPluginVariables) s += "| plugin " + v.Name + "=" + v.Value;
        return s;
    }

    static void Shot(string name)
    {
        using (Bitmap bmp = new Bitmap(page.Width, page.Height))
        {
            page.DrawToBitmap(bmp, new Rectangle(0, 0, page.Width, page.Height));
            bmp.Save(System.IO.Path.Combine(outDir, name + ".png"), ImageFormat.Png);
        }
    }

    static void Click(string box)
    {
        CheckBox b = Box(box);
        P("click " + box + " (" + (b.Checked ? "on" : "off") + " -> " + (b.Checked ? "off" : "on") + ")");
        b.Checked = !b.Checked;  // what a click does; its CheckedChanged handler sends the command
    }

    // Steps run on the UI thread, a few seconds apart, so the network replies can land in between.
    static List<KeyValuePair<int, Action>> steps = new List<KeyValuePair<int, Action>>();
    static void Step(int afterMs, Action a) { steps.Add(new KeyValuePair<int, Action>(afterMs, a)); }

    [STAThread]
    static void Main(string[] args)
    {
        outDir = args.Length > 0 ? args[0] : ".";
        app = new PRoConApplication(true, new string[0]);
        Assembly gui = Assembly.LoadFrom("PRoCon.exe");
        Type pageType = gui.GetType("PRoCon.Controls.ServerSettings.MOHW.uscServerSettingsGameplayMOHW", true);

        form = new Form { Width = 1000, Height = 720, Text = "harness3", ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-2000, -2000) };
        page = (Control)Activator.CreateInstance(pageType);
        page.Dock = DockStyle.Fill;
        form.Controls.Add(page);
        form.Show();
        P("page built: Hardcore box " + (Box("chkSettingsHardcore").Enabled ? "clickable" : "greyed"));

        app.Execute();
        foreach (PRoConClient x in app.Connections) c = x;
        pageType.GetMethod("SetLocalization").Invoke(page, new object[] { app.CurrentLanguage });
        pageType.GetMethod("SetConnection").Invoke(page, new object[] { c });
        c.GameTypeDiscovered += (s) =>
        {
            s.Game.ResponseError += (g, a, b) => P("error reply: " + string.Join(" ", a.Words.ToArray()) + " => " + b);
        };

        Step(9000, () =>
        {
            if (c.PluginsManager == null || !c.PluginsManager.Plugins.IsLoaded("CMohwBots")) { P("PLUGIN NOT LOADED"); Application.Exit(); return; }
            P("connected, plugin loaded, not enabled: " + State());
            Shot("1-plugin-off");
            c.PluginsManager.EnablePlugin("CMohwBots");
        });
        if (args.Length > 1 && args[1] == "old")
        {
            Step(3000, () => { P("plugin enabled, old game: " + State()); Shot("old-1"); c.PluginsManager.SetPluginVariable("CMohwBots", "Hardcore", "Yes"); });
            Step(3000, () => { c.SendRequest(new List<string> { "serverInfo" }); });
            Step(2000, () => { P("after the plugin's switch, on: " + State() + " server name " + c.CurrentServerInfo.ServerName); c.PluginsManager.SetPluginVariable("CMohwBots", "Hardcore", "No"); });
            Step(3000, () => { c.SendRequest(new List<string> { "serverInfo" }); });
            Step(2000, () => { P("after the plugin's switch, off: " + State() + " server name " + c.CurrentServerInfo.ServerName); });
        }
        else
        {
        Step(3000, () => { P("plugin enabled: " + State()); Shot("2-plugin-on"); Click("chkSettingsHardcore"); });
        Step(5000, () => { P("after the Hardcore box, on: " + State()); Shot("3-hardcore-on"); Click("chkSettingsKillCam"); });
        Step(2500, () => { P("after Killcam while Hardcore is on: " + State()); Click("chkSettingsHardcore"); });
        Step(5000, () => { P("after the Hardcore box, off: " + State()); Shot("4-hardcore-off"); c.PluginsManager.SetPluginVariable("CMohwBots", "Hardcore", "Yes"); });
        Step(5000, () => { P("after the plugin's own switch, on: " + State()); c.SendRequest(new List<string> { "vars.preset", "normal" }); });
        Step(2000, () => { c.SendRequest(new List<string> { "serverInfo" }); });
        Step(5000, () => { P("after vars.preset normal in the console and a server info: " + State()); Shot("5-console-off"); });
        }

        int at = 0;
        System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 250 };
        DateTime start = DateTime.Now;
        int due = steps[0].Key;
        timer.Tick += (s, e) =>
        {
            if (at >= steps.Count) { timer.Stop(); P("done"); app.Shutdown(); Application.Exit(); return; }
            if ((DateTime.Now - start).TotalMilliseconds < due) return;
            try { steps[at].Value(); } catch (Exception ex) { P("step failed: " + ex); }
            at++;
            start = DateTime.Now;
            due = at < steps.Count ? steps[at].Key : 0;
        };
        timer.Start();
        Application.Run(form);
        Environment.Exit(0);
    }
}
