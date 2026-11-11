using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Reflection;
using System.Windows.Forms;
using PRoCon.Core;
using PRoCon.Core.Remote;

// Puts ProCon's own Maplist tab (PRoCon.Controls.Maplist.uscMaplist, from PRoCon.exe) in a hidden window, connected through the
// real PRoCon.Core to the test server, and uses it the way a person would: pick a mode (ProCon calls it a playlist), click
// "change playlist", add a map from the pool, set the next map, remove one. Logs what the tab shows; screenshots go to args[0].
class MaplistTest
{
    static PRoConApplication app;
    static PRoConClient c;
    static Control page;
    static Type pageType;
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

    static void Call(string method)
    {
        P("click: " + method);
        pageType.GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic).Invoke(page, new object[] { page, EventArgs.Empty });
    }

    static string Rows(string listName)
    {
        ListView list = (ListView)Field(listName);
        string s = "";
        foreach (ListViewItem item in list.Items)
        {
            s += "\n    [";
            foreach (ListViewItem.ListViewSubItem sub in item.SubItems) s += sub.Text + " | ";
            s += "]";
        }
        return list.Items.Count + " rows" + s;
    }

    static string State()
    {
        ComboBox modes = (ComboBox)Field("cboMaplistPlaylists");
        string items = "";
        foreach (object o in modes.Items) items += ((CMap)o).PlayList + " ";
        return "playlists [" + items + "] selected " + (modes.SelectedItem == null ? "none" : ((CMap)modes.SelectedItem).PlayList) +
               "; server playlist \"" + c.ListSettings.CurrentPlaylist + "\"; add panel " + (((Control)Field("pnlMaplistAddMap")).Enabled ? "enabled" : "DISABLED") +
               "; must-change warning " + (((Control)Field("lblMaplistMustChangeWarning")).Visible ? "shown" : "hidden");
    }

    static void SelectPlaylist(string playlist)
    {
        ComboBox modes = (ComboBox)Field("cboMaplistPlaylists");
        foreach (object o in modes.Items)
            if (((CMap)o).PlayList == playlist) { P("pick playlist " + playlist); modes.SelectedItem = o; return; }
        P("NO PLAYLIST " + playlist);
    }

    // The row whose cells include every one of texts (the pool shows mode, map code, map name).
    static void Select(string listName, params string[] texts)
    {
        ListView list = (ListView)Field(listName);
        bool found = false;
        foreach (ListViewItem item in list.Items)
        {
            int hits = 0;
            foreach (string text in texts)
                foreach (ListViewItem.ListViewSubItem sub in item.SubItems)
                    if (sub.Text == text) { hits++; break; }
            item.Selected = !found && hits == texts.Length;
            found |= item.Selected;
        }
        P((found ? "select " : "NOT FOUND ") + string.Join(" / ", texts) + " in " + listName);
    }

    // What a right-click on a row does before the menu's "Set next map" (lsvMaplist_MouseDown tags the menu item).
    static void SetNext(int index)
    {
        ((ToolStripMenuItem)Field("setNextMapToolStripMenuItem")).Tag = index;
        Call("setNextMapToolStripMenuItem_Click");
    }

    static void SelectIndex(string listName, int index)
    {
        ListView list = (ListView)Field(listName);
        for (int i = 0; i < list.Items.Count; ++i) list.Items[i].Selected = i == index;
        P("select row " + index + " in " + listName);
    }

    static void Shot(string name)
    {
        using (Bitmap bmp = new Bitmap(page.Width, page.Height))
        {
            page.DrawToBitmap(bmp, new Rectangle(0, 0, page.Width, page.Height));
            bmp.Save(System.IO.Path.Combine(outDir, name + ".png"), ImageFormat.Png);
        }
    }

    static List<KeyValuePair<int, Action>> steps = new List<KeyValuePair<int, Action>>();
    static void Step(int afterMs, Action a) { steps.Add(new KeyValuePair<int, Action>(afterMs, a)); }

    [STAThread]
    static void Main(string[] args)
    {
        outDir = args.Length > 0 ? args[0] : ".";
        app = new PRoConApplication(true, new string[0]);
        Assembly gui = Assembly.LoadFrom("PRoCon.exe");
        pageType = gui.GetType("PRoCon.Controls.Maplist.uscMaplist", true);

        Form form = new Form { Width = 1100, Height = 700, Text = "maplist-test", ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-2000, -2000) };
        page = (Control)Activator.CreateInstance(pageType);
        page.Dock = DockStyle.Fill;
        form.Controls.Add(page);
        form.Show();

        app.Execute();
        foreach (PRoConClient x in app.Connections) c = x;
        pageType.GetMethod("SetLocalization").Invoke(page, new object[] { app.CurrentLanguage });
        pageType.GetMethod("SetConnection").Invoke(page, new object[] { c });
        c.GameTypeDiscovered += (s) =>
        {
            s.Game.ResponseError += (g, a, b) => P("error reply: " + string.Join(" ", a.Words.ToArray()) + " => " + b);
        };

        // For MOHW ProCon's map list is "open" (MOHWClient.HasOpenMaplist): the pool lists every mode's maps and there's no
        // playlist to pick, so the add panel is open from the start.
        Step(9000, () => { P("connected: " + State()); P("map list: " + Rows("lsvMaplist")); P("pool: " + Rows("lsvMaplistPool")); Shot("maplist-1-connected"); });
        Step(500, () => { Select("lsvMaplistPool", "Sector Control", "MP_12"); Call("btnAddMap_Click"); });
        Step(2500, () => { P("after adding Sector Control on Basilan: " + Rows("lsvMaplist")); Shot("maplist-2-added"); Select("lsvMaplistPool", "Home Run", "MP_10"); Call("btnAddMap_Click"); });
        Step(2500, () => { P("after adding Home Run on Sarajevo: " + Rows("lsvMaplist")); Select("lsvMaplistPool", "Hot Spot", "MP_21"); Call("btnAddMap_Click"); });
        Step(2500, () => { P("after adding Hot Spot on Chitrail (a map the bots haven't played): " + Rows("lsvMaplist")); SetNext(3); });
        Step(2500, () => { P("after set next: " + Rows("lsvMaplist")); SelectIndex("lsvMaplist", 0); Call("btnRemoveMap_Click"); });
        Step(2500, () => { P("after remove row 0: " + Rows("lsvMaplist")); Shot("maplist-4-final"); Call("btnMaplistRefresh_Click"); });
        Step(2500, () => { P("after refresh: " + Rows("lsvMaplist")); });

        int at = 0;
        Timer timer = new Timer { Interval = 250 };
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
