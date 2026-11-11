using System;
using System.Collections.Generic;

using PRoCon.Core;
using PRoCon.Core.Plugin;

// ProCon's Server Settings > Gameplay page has a Hardcore box, but for Warfighter ProCon never asks the server about it, so the box
// stays greyed out. This plugin asks (vars.hardCore), which unlocks the box, and asks again whenever Hardcore changes, so the box and
// the settings Hardcore changes stay current. It also has its own Hardcore switch, a view of the game's state: the game is the
// authority, and says so in vars.hardCore and by adding [Hardcore] to the server name.
namespace PRoConEvents {
    public class CMohwBots : PRoConPluginAPI, IPRoConPluginInterface {
        private const string HardcoreVariable = "Hardcore";
        private const string ServerNameMark = "[Hardcore]";

        // What ProCon's own panels show and Hardcore changes. ProCon asks for them once at login, so they're asked again after a switch.
        private static readonly string[] ChangedSettings = {
            "vars.friendlyFire", "vars.regenerateHealth", "vars.soldierHealth", "vars.killCam", "vars.nameTag", "vars.onlySquadLeaderSpawn",
            "vars.buddyOutline", "vars.hudBuddyInfo", "vars.hudClassAbility", "vars.hudCrosshair", "vars.hudEnemyTag", "vars.hudExplosiveIcons",
            "vars.hudGameMode", "vars.hudHealthAmmo", "vars.hudMinimap", "vars.hudObiturary", "vars.hudPointsTracker", "vars.hudUnlocks"
        };

        private bool m_enabled = false;
        private bool m_serverKnown = false;
        private bool m_oldGame = false;  // its mohw-bots doesn't know vars.hardCore yet: go by the server name, switch with vars.preset
        private enumBoolYesNo m_hardcore = enumBoolYesNo.No;

        public string GetPluginName() { return "MOHW Bots"; }
        public string GetPluginVersion() { return "1.1.0"; }
        public string GetPluginAuthor() { return "mohw-bots"; }
        public string GetPluginWebsite() { return ""; }

        public string GetPluginDescription() {
            return "<h2>MOHW Bots</h2><p>Lets you turn Hardcore mode on or off in your Medal of Honor: Warfighter bot match.</p>"
                 + "<p>While this plugin is enabled, the Hardcore box on Server Settings &gt; Gameplay works, and the boxes it changes there "
                 + "update after a switch. The switch below does the same thing.</p>"
                 + "<p>Friendly fire, health regeneration, the HUD and spotting change within a couple of seconds. "
                 + "Soldier health changes the next time each soldier spawns. Your choice is saved in mohw-bots.ini, so the next launch keeps it.</p>"
                 + "<p>You can still change single settings on the Gameplay page while Hardcore is on; they stay as you set them until "
                 + "Hardcore is switched again.</p>";
        }

        public void OnPluginLoadingEnv(List<string> lstPluginEnv) { }

        public void OnPluginLoaded(string strHostName, string strPort, string strPRoConVersion) {
            this.RegisterEvents(this.GetType().Name, "OnServerInfo", "OnHardcore", "OnLogin", "OnResponseError");
        }

        public void OnPluginEnable() {
            m_enabled = true;
            m_serverKnown = false;  // the game's answer says where things stand
            Send("vars.hardCore");
            Send("serverInfo");
        }

        public void OnPluginDisable() {
            m_enabled = false;
            m_serverKnown = false;
            this.ExecuteCommand("procon.protected.tasks.remove", "CMohwBots");
        }

        // A new connection (the game was restarted): ProCon greys the box again until someone asks.
        public override void OnLogin() {
            if (!m_enabled)
                return;
            m_serverKnown = false;
            m_oldGame = false;  // it may have been updated
            Send("vars.hardCore");
        }

        public override void OnResponseError(List<string> requestWords, string error) {
            if (!m_enabled || requestWords == null || requestWords.Count == 0 || requestWords[0] != "vars.hardCore")
                return;
            if (!m_oldGame)
                this.ExecuteCommand("procon.protected.pluginconsole.write",
                    "^bMOHW Bots^n: the game's mohw-bots is older than this plugin, so the Hardcore box on the Gameplay page won't show the right state. The switch here still works. Install the xinput1_3.dll that came with this plugin to fix it.");
            m_oldGame = true;
            if (requestWords.Count > 1)
                Send("vars.preset", requestWords[1] == "true" ? "hardcore" : "normal");
        }

        public List<CPluginVariable> GetDisplayPluginVariables() {
            List<CPluginVariable> list = new List<CPluginVariable>();
            list.Add(new CPluginVariable("Game mode|" + HardcoreVariable, typeof(enumBoolYesNo), m_hardcore));
            return list;
        }

        public List<CPluginVariable> GetPluginVariables() {
            List<CPluginVariable> list = new List<CPluginVariable>();
            list.Add(new CPluginVariable(HardcoreVariable, typeof(enumBoolYesNo), m_hardcore));
            return list;
        }

        // ProCon also calls this with the saved value when it starts. That must not change the game, so nothing is sent until the game
        // has said where it stands, and only for a real change.
        public void SetPluginVariable(string strVariable, string strValue) {
            if (strVariable != HardcoreVariable || !Enum.IsDefined(typeof(enumBoolYesNo), strValue))
                return;
            enumBoolYesNo wanted = (enumBoolYesNo)Enum.Parse(typeof(enumBoolYesNo), strValue);
            bool changed = wanted != m_hardcore;
            m_hardcore = wanted;
            if (!changed || !m_enabled || !m_serverKnown)
                return;
            // vars.hardCore rather than vars.preset: ProCon reads the reply, so the Gameplay page's box follows too.
            Send("vars.hardCore", wanted == enumBoolYesNo.Yes ? "true" : "false");
            Said(wanted == enumBoolYesNo.Yes);
            AskAgainSoon();
        }

        // ProCon read a vars.hardCore reply: ours, or the Gameplay page's box being clicked.
        public override void OnHardcore(bool isEnabled) {
            if (!m_enabled)
                return;
            Learned(isEnabled);
        }

        // About every 30 s. Catches a switch made some other way (vars.preset in the console): the answer goes to OnHardcore.
        public override void OnServerInfo(CServerInfo serverInfo) {
            if (!m_enabled || serverInfo == null || serverInfo.ServerName == null)
                return;
            bool marked = serverInfo.ServerName.Contains(ServerNameMark);
            if (m_oldGame)
                Learned(marked);
            else if (!m_serverKnown || marked != (m_hardcore == enumBoolYesNo.Yes))
                Send("vars.hardCore");
        }

        private void Learned(bool on) {
            bool changed = m_serverKnown && on != (m_hardcore == enumBoolYesNo.Yes);
            m_hardcore = on ? enumBoolYesNo.Yes : enumBoolYesNo.No;
            m_serverKnown = true;
            if (!changed)
                return;
            Said(on);
            AskAgainSoon();
        }

        private void Said(bool on) {
            this.ExecuteCommand("procon.protected.pluginconsole.write", "^bMOHW Bots^n: Hardcore " + (on ? "^2on" : "^1off"));
        }

        // The game applies a switch within a couple of seconds.
        private void AskAgainSoon() {
            foreach (string setting in ChangedSettings)
                this.ExecuteCommand("procon.protected.tasks.add", "CMohwBots", "3", "1", "1", "procon.protected.send", setting);
        }

        private void Send(params string[] words) {
            string[] command = new string[words.Length + 1];
            command[0] = "procon.protected.send";
            words.CopyTo(command, 1);
            this.ExecuteCommand(command);
        }
    }
}
