using System;
using System.Windows.Forms;

partial class Form1
{
    private System.ComponentModel.IContainer components = null;

    private PictureBox banner;
    private RadioButton radioFullscreenYes;
    private RadioButton radioFullscreenNo;
    private RadioButton radioVsyncYes;
    private RadioButton radioVsyncNo;
    private ComboBox comboMap;
    private Button btnPlay;

    private ComboBox comboResolution;
    private ComboBox comboRefresh;
    private RadioButton radioCullingYes;
    private RadioButton radioCullingNo;
    private RadioButton radioPreloadYes;
    private RadioButton radioPreloadNo;
    private RadioButton introYes;
    private RadioButton introNo;

    private Label cullLabel;
    private Label fullscreenLabel;
    private Label vsyncLabel;
    private Label resolutionLabel;
    private Label refreshLabel;
    private Label levelLabel;
    private Label introLabel;

    private Panel fullscreenPanel;
    private Panel vsyncPanel;
    private Panel cullingPanel;
    private Panel preloadPanel;
    private Panel introPanel;

    private Label fpsLabel;
    private ComboBox comboFps;

    private Label filteringLabel;
    private ComboBox comboFiltering;

    private Label loggingLabel;
    private Panel loggingPanel;
    private RadioButton loggingYes;
    private RadioButton loggingNo;

    private Label consoleLabel;
    private Panel consolePanel;
    private RadioButton consoleYes;
    private RadioButton consoleNo;

    private Label looseLabel;
    private Panel loosePanel;
    private RadioButton looseYes;
    private RadioButton looseNo;
    private ToolTip looseTooltip;



    protected override void Dispose(bool disposing)
    {
        if (disposing && (components != null))
            components.Dispose();
        base.Dispose(disposing);
    }

    private void InitializeComponent()
    {
            this.fullscreenPanel = new System.Windows.Forms.Panel();
            this.radioFullscreenYes = new System.Windows.Forms.RadioButton();
            this.radioFullscreenNo = new System.Windows.Forms.RadioButton();
            this.vsyncPanel = new System.Windows.Forms.Panel();
            this.radioVsyncYes = new System.Windows.Forms.RadioButton();
            this.radioVsyncNo = new System.Windows.Forms.RadioButton();
            this.cullingPanel = new System.Windows.Forms.Panel();
            this.radioCullingYes = new System.Windows.Forms.RadioButton();
            this.radioCullingNo = new System.Windows.Forms.RadioButton();
            this.preloadPanel = new System.Windows.Forms.Panel();
            this.radioPreloadYes = new System.Windows.Forms.RadioButton();
            this.radioPreloadNo = new System.Windows.Forms.RadioButton();
            this.introYes = new System.Windows.Forms.RadioButton();
            this.introNo = new System.Windows.Forms.RadioButton();
            this.introPanel = new System.Windows.Forms.Panel();
            this.comboMap = new System.Windows.Forms.ComboBox();
            this.btnPlay = new System.Windows.Forms.Button();
            this.comboResolution = new System.Windows.Forms.ComboBox();
            this.comboRefresh = new System.Windows.Forms.ComboBox();
            this.banner = new System.Windows.Forms.PictureBox();
            this.cullLabel = new System.Windows.Forms.Label();
            this.fullscreenLabel = new System.Windows.Forms.Label();
            this.vsyncLabel = new System.Windows.Forms.Label();
            this.resolutionLabel = new System.Windows.Forms.Label();
            this.refreshLabel = new System.Windows.Forms.Label();
            this.levelLabel = new System.Windows.Forms.Label();
            this.introLabel = new System.Windows.Forms.Label();
            this.fpsLabel = new System.Windows.Forms.Label();
            this.chunksLabel = new System.Windows.Forms.Label();
            this.comboFps = new System.Windows.Forms.ComboBox();
            this.filteringLabel = new System.Windows.Forms.Label();
            this.comboFiltering = new System.Windows.Forms.ComboBox();
            this.loggingLabel = new System.Windows.Forms.Label();
            this.loggingPanel = new System.Windows.Forms.Panel();
            this.loggingYes = new System.Windows.Forms.RadioButton();
            this.loggingNo = new System.Windows.Forms.RadioButton();
            this.consoleLabel = new System.Windows.Forms.Label();
            this.consolePanel = new System.Windows.Forms.Panel();
            this.consoleYes = new System.Windows.Forms.RadioButton();
            this.consoleNo = new System.Windows.Forms.RadioButton();
            this.looseLabel = new System.Windows.Forms.Label();
            this.loosePanel = new System.Windows.Forms.Panel();
            this.looseYes = new System.Windows.Forms.RadioButton();
            this.looseNo = new System.Windows.Forms.RadioButton();
            this.looseTooltip = new System.Windows.Forms.ToolTip(this.components);
            this.fullscreenPanel.SuspendLayout();
            this.vsyncPanel.SuspendLayout();
            this.cullingPanel.SuspendLayout();
            this.preloadPanel.SuspendLayout();
            this.introPanel.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.banner)).BeginInit();
            this.loggingPanel.SuspendLayout();
            this.consolePanel.SuspendLayout();
            this.loosePanel.SuspendLayout();
            this.SuspendLayout();
            // 
            // fullscreenPanel
            // 
            this.fullscreenPanel.Controls.Add(this.radioFullscreenYes);
            this.fullscreenPanel.Controls.Add(this.radioFullscreenNo);
            this.fullscreenPanel.Location = new System.Drawing.Point(80, 116);
            this.fullscreenPanel.Name = "fullscreenPanel";
            this.fullscreenPanel.Size = new System.Drawing.Size(120, 30);
            this.fullscreenPanel.TabIndex = 16;
            // 
            // radioFullscreenYes
            // 
            this.radioFullscreenYes.Location = new System.Drawing.Point(5, 5);
            this.radioFullscreenYes.Name = "radioFullscreenYes";
            this.radioFullscreenYes.Size = new System.Drawing.Size(51, 24);
            this.radioFullscreenYes.TabIndex = 7;
            this.radioFullscreenYes.Text = "Yes";
            // 
            // radioFullscreenNo
            // 
            this.radioFullscreenNo.Location = new System.Drawing.Point(60, 5);
            this.radioFullscreenNo.Name = "radioFullscreenNo";
            this.radioFullscreenNo.Size = new System.Drawing.Size(47, 24);
            this.radioFullscreenNo.TabIndex = 8;
            this.radioFullscreenNo.Text = "No";
            // 
            // vsyncPanel
            // 
            this.vsyncPanel.Controls.Add(this.radioVsyncYes);
            this.vsyncPanel.Controls.Add(this.radioVsyncNo);
            this.vsyncPanel.Location = new System.Drawing.Point(80, 176);
            this.vsyncPanel.Name = "vsyncPanel";
            this.vsyncPanel.Size = new System.Drawing.Size(120, 30);
            this.vsyncPanel.TabIndex = 17;
            // 
            // radioVsyncYes
            // 
            this.radioVsyncYes.Location = new System.Drawing.Point(5, 5);
            this.radioVsyncYes.Name = "radioVsyncYes";
            this.radioVsyncYes.Size = new System.Drawing.Size(51, 24);
            this.radioVsyncYes.TabIndex = 9;
            this.radioVsyncYes.Text = "Yes";
            // 
            // radioVsyncNo
            // 
            this.radioVsyncNo.Location = new System.Drawing.Point(60, 5);
            this.radioVsyncNo.Name = "radioVsyncNo";
            this.radioVsyncNo.Size = new System.Drawing.Size(47, 24);
            this.radioVsyncNo.TabIndex = 10;
            this.radioVsyncNo.Text = "No";
            // 
            // cullingPanel
            // 
            this.cullingPanel.Controls.Add(this.radioCullingYes);
            this.cullingPanel.Controls.Add(this.radioCullingNo);
            this.cullingPanel.Location = new System.Drawing.Point(300, 116);
            this.cullingPanel.Name = "cullingPanel";
            this.cullingPanel.Size = new System.Drawing.Size(100, 30);
            this.cullingPanel.TabIndex = 18;
            // 
            // radioCullingYes
            // 
            this.radioCullingYes.Location = new System.Drawing.Point(5, 5);
            this.radioCullingYes.Name = "radioCullingYes";
            this.radioCullingYes.Size = new System.Drawing.Size(45, 24);
            this.radioCullingYes.TabIndex = 2;
            this.radioCullingYes.Text = "Yes";
            this.radioCullingYes.CheckedChanged += new System.EventHandler(this.radioCullingYes_CheckedChanged);
            // 
            // radioCullingNo
            // 
            this.radioCullingNo.Location = new System.Drawing.Point(59, 5);
            this.radioCullingNo.Name = "radioCullingNo";
            this.radioCullingNo.Size = new System.Drawing.Size(40, 24);
            this.radioCullingNo.TabIndex = 3;
            this.radioCullingNo.Text = "No";
            // 
            // preloadPanel
            // 
            this.preloadPanel.Controls.Add(this.radioPreloadYes);
            this.preloadPanel.Controls.Add(this.radioPreloadNo);
            this.preloadPanel.Location = new System.Drawing.Point(300, 148);
            this.preloadPanel.Name = "preloadPanel";
            this.preloadPanel.Size = new System.Drawing.Size(100, 30);
            this.preloadPanel.TabIndex = 20;
            this.preloadPanel.Paint += new System.Windows.Forms.PaintEventHandler(this.preloadPanel_Paint);
            // 
            // radioPreloadYes
            // 
            this.radioPreloadYes.Location = new System.Drawing.Point(4, 3);
            this.radioPreloadYes.Name = "radioPreloadYes";
            this.radioPreloadYes.Size = new System.Drawing.Size(45, 24);
            this.radioPreloadYes.TabIndex = 4;
            this.radioPreloadYes.Text = "Yes";
            this.radioPreloadYes.CheckedChanged += new System.EventHandler(this.radioPreloadYes_CheckedChanged);
            // 
            // radioPreloadNo
            // 
            this.radioPreloadNo.Location = new System.Drawing.Point(59, 3);
            this.radioPreloadNo.Name = "radioPreloadNo";
            this.radioPreloadNo.Size = new System.Drawing.Size(53, 24);
            this.radioPreloadNo.TabIndex = 5;
            this.radioPreloadNo.Text = "No";
            // 
            // introYes
            // 
            this.introYes.Location = new System.Drawing.Point(5, 5);
            this.introYes.Name = "introYes";
            this.introYes.Size = new System.Drawing.Size(49, 24);
            this.introYes.TabIndex = 6;
            this.introYes.Text = "Yes";
            this.introYes.CheckedChanged += new System.EventHandler(this.introYes_CheckedChanged);
            // 
            // introNo
            // 
            this.introNo.Location = new System.Drawing.Point(59, 5);
            this.introNo.Name = "introNo";
            this.introNo.Size = new System.Drawing.Size(45, 24);
            this.introNo.TabIndex = 7;
            this.introNo.Text = "No";
            // 
            // introPanel
            // 
            this.introPanel.Controls.Add(this.introYes);
            this.introPanel.Controls.Add(this.introNo);
            this.introPanel.Location = new System.Drawing.Point(299, 180);
            this.introPanel.Name = "introPanel";
            this.introPanel.Size = new System.Drawing.Size(138, 30);
            this.introPanel.TabIndex = 20;
            this.introPanel.Paint += new System.Windows.Forms.PaintEventHandler(this.introPanel_Paint);
            //
            // comboMap
            //
            this.comboMap.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboMap.Location = new System.Drawing.Point(8, 345);
            this.comboMap.Name = "comboMap";
            this.comboMap.Size = new System.Drawing.Size(200, 21);
            this.comboMap.TabIndex = 11;
            this.comboMap.SelectedIndexChanged += new System.EventHandler(this.comboMap_SelectedIndexChanged);
            // 
            // btnPlay
            // 
            this.btnPlay.Location = new System.Drawing.Point(272, 345);
            this.btnPlay.Name = "btnPlay";
            this.btnPlay.Size = new System.Drawing.Size(116, 23);
            this.btnPlay.TabIndex = 12;
            this.btnPlay.Text = "Play";
            this.btnPlay.Click += new System.EventHandler(this.btnPlay_Click);
            //
            // comboResolution
            //
            this.comboResolution.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboResolution.Location = new System.Drawing.Point(80, 151);
            this.comboResolution.Name = "comboResolution";
            this.comboResolution.Size = new System.Drawing.Size(120, 21);
            this.comboResolution.TabIndex = 0;
            this.comboResolution.SelectedIndexChanged += new System.EventHandler(this.comboResolution_SelectedIndexChanged);
            //
            // comboRefresh
            //
            this.comboRefresh.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboRefresh.Location = new System.Drawing.Point(80, 211);
            this.comboRefresh.Name = "comboRefresh";
            this.comboRefresh.Size = new System.Drawing.Size(120, 21);
            this.comboRefresh.TabIndex = 1;
            this.comboRefresh.SelectedIndexChanged += new System.EventHandler(this.comboRefresh_SelectedIndexChanged);
            // 
            // banner
            // 
            this.banner.BackColor = System.Drawing.Color.DarkSlateGray;
            this.banner.Dock = System.Windows.Forms.DockStyle.Top;
            this.banner.Image = global::SilentHillPC_Launcher.Properties.Resources.launcher;
            this.banner.Location = new System.Drawing.Point(0, 0);
            this.banner.Name = "banner";
            this.banner.Size = new System.Drawing.Size(400, 111);
            this.banner.SizeMode = System.Windows.Forms.PictureBoxSizeMode.StretchImage;
            this.banner.TabIndex = 6;
            this.banner.TabStop = false;
            // 
            // cullLabel
            // 
            this.cullLabel.AutoSize = true;
            this.cullLabel.Location = new System.Drawing.Point(214, 125);
            this.cullLabel.Name = "cullLabel";
            this.cullLabel.Size = new System.Drawing.Size(79, 13);
            this.cullLabel.TabIndex = 0;
            this.cullLabel.Text = "Disable Culling:";
            // 
            // fullscreenLabel
            // 
            this.fullscreenLabel.AutoSize = true;
            this.fullscreenLabel.Location = new System.Drawing.Point(6, 125);
            this.fullscreenLabel.Name = "fullscreenLabel";
            this.fullscreenLabel.Size = new System.Drawing.Size(58, 13);
            this.fullscreenLabel.TabIndex = 2;
            this.fullscreenLabel.Text = "Fullscreen:";
            // 
            // vsyncLabel
            // 
            this.vsyncLabel.AutoSize = true;
            this.vsyncLabel.Location = new System.Drawing.Point(8, 185);
            this.vsyncLabel.Name = "vsyncLabel";
            this.vsyncLabel.Size = new System.Drawing.Size(41, 13);
            this.vsyncLabel.TabIndex = 3;
            this.vsyncLabel.Text = "VSync:";
            // 
            // resolutionLabel
            // 
            this.resolutionLabel.AutoSize = true;
            this.resolutionLabel.Location = new System.Drawing.Point(6, 155);
            this.resolutionLabel.Name = "resolutionLabel";
            this.resolutionLabel.Size = new System.Drawing.Size(60, 13);
            this.resolutionLabel.TabIndex = 13;
            this.resolutionLabel.Text = "Resolution:";
            // 
            // refreshLabel
            // 
            this.refreshLabel.AutoSize = true;
            this.refreshLabel.Location = new System.Drawing.Point(7, 215);
            this.refreshLabel.Name = "refreshLabel";
            this.refreshLabel.Size = new System.Drawing.Size(73, 13);
            this.refreshLabel.TabIndex = 14;
            this.refreshLabel.Text = "Refresh Rate:";
            // 
            // levelLabel
            // 
            this.levelLabel.AutoSize = true;
            this.levelLabel.Location = new System.Drawing.Point(8, 329);
            this.levelLabel.Name = "levelLabel";
            this.levelLabel.Size = new System.Drawing.Size(36, 13);
            this.levelLabel.TabIndex = 15;
            this.levelLabel.Text = "Level:";
            // 
            // introLabel
            // 
            this.introLabel.AutoSize = true;
            this.introLabel.Location = new System.Drawing.Point(214, 189);
            this.introLabel.Name = "introLabel";
            this.introLabel.Size = new System.Drawing.Size(60, 13);
            this.introLabel.TabIndex = 16;
            this.introLabel.Text = "Skip Intros:";
            // 
            // fpsLabel
            // 
            this.fpsLabel.AutoSize = true;
            this.fpsLabel.Location = new System.Drawing.Point(8, 245);
            this.fpsLabel.Name = "fpsLabel";
            this.fpsLabel.Size = new System.Drawing.Size(54, 13);
            this.fpsLabel.TabIndex = 30;
            this.fpsLabel.Text = "FPS Limit:";
            // 
            // chunksLabel
            // 
            this.chunksLabel.AutoSize = true;
            this.chunksLabel.Location = new System.Drawing.Point(214, 157);
            this.chunksLabel.Name = "chunksLabel";
            this.chunksLabel.Size = new System.Drawing.Size(85, 13);
            this.chunksLabel.TabIndex = 1;
            this.chunksLabel.Text = "Preload Chunks:";
            //
            // comboFps
            //
            this.comboFps.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboFps.Items.AddRange(new object[] {
            "0",
            "30",
            "60",
            "120",
            "240"});
            this.comboFps.Location = new System.Drawing.Point(80, 241);
            this.comboFps.Name = "comboFps";
            this.comboFps.Size = new System.Drawing.Size(120, 21);
            this.comboFps.TabIndex = 31;
            //
            // filteringLabel
            //
            this.filteringLabel.AutoSize = true;
            this.filteringLabel.Location = new System.Drawing.Point(8, 275);
            this.filteringLabel.Name = "filteringLabel";
            this.filteringLabel.Size = new System.Drawing.Size(50, 13);
            this.filteringLabel.TabIndex = 40;
            this.filteringLabel.Text = "Filtering:";
            //
            // comboFiltering
            //
            this.comboFiltering.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboFiltering.Items.AddRange(new object[] {
            "Off",
            "Dithering",
            "Bilinear"});
            this.comboFiltering.Location = new System.Drawing.Point(80, 271);
            this.comboFiltering.Name = "comboFiltering";
            this.comboFiltering.Size = new System.Drawing.Size(120, 21);
            this.comboFiltering.TabIndex = 41;
            // 
            // loggingLabel
            // 
            this.loggingLabel.AutoSize = true;
            this.loggingLabel.Location = new System.Drawing.Point(214, 221);
            this.loggingLabel.Name = "loggingLabel";
            this.loggingLabel.Size = new System.Drawing.Size(84, 13);
            this.loggingLabel.TabIndex = 32;
            this.loggingLabel.Text = "Enable Logging:";
            // 
            // loggingPanel
            // 
            this.loggingPanel.Controls.Add(this.loggingYes);
            this.loggingPanel.Controls.Add(this.loggingNo);
            this.loggingPanel.Location = new System.Drawing.Point(299, 212);
            this.loggingPanel.Name = "loggingPanel";
            this.loggingPanel.Size = new System.Drawing.Size(138, 30);
            this.loggingPanel.TabIndex = 35;
            // 
            // loggingYes
            // 
            this.loggingYes.Location = new System.Drawing.Point(5, 5);
            this.loggingYes.Name = "loggingYes";
            this.loggingYes.Size = new System.Drawing.Size(49, 24);
            this.loggingYes.TabIndex = 33;
            this.loggingYes.Text = "Yes";
            // 
            // loggingNo
            // 
            this.loggingNo.Location = new System.Drawing.Point(59, 5);
            this.loggingNo.Name = "loggingNo";
            this.loggingNo.Size = new System.Drawing.Size(45, 24);
            this.loggingNo.TabIndex = 34;
            this.loggingNo.Text = "No";
            // 
            // consoleLabel
            // 
            this.consoleLabel.AutoSize = true;
            this.consoleLabel.Location = new System.Drawing.Point(214, 253);
            this.consoleLabel.Name = "consoleLabel";
            this.consoleLabel.Size = new System.Drawing.Size(78, 13);
            this.consoleLabel.TabIndex = 36;
            this.consoleLabel.Text = "Show Console:";
            // 
            // consolePanel
            // 
            this.consolePanel.Controls.Add(this.consoleYes);
            this.consolePanel.Controls.Add(this.consoleNo);
            this.consolePanel.Location = new System.Drawing.Point(299, 244);
            this.consolePanel.Name = "consolePanel";
            this.consolePanel.Size = new System.Drawing.Size(138, 30);
            this.consolePanel.TabIndex = 39;
            // 
            // consoleYes
            // 
            this.consoleYes.Location = new System.Drawing.Point(5, 5);
            this.consoleYes.Name = "consoleYes";
            this.consoleYes.Size = new System.Drawing.Size(49, 24);
            this.consoleYes.TabIndex = 37;
            this.consoleYes.Text = "Yes";
            // 
            // consoleNo
            // 
            this.consoleNo.Location = new System.Drawing.Point(59, 5);
            this.consoleNo.Name = "consoleNo";
            this.consoleNo.Size = new System.Drawing.Size(45, 24);
            this.consoleNo.TabIndex = 38;
            this.consoleNo.Text = "No";
            //
            // looseLabel
            //
            this.looseLabel.AutoSize = true;
            this.looseLabel.Location = new System.Drawing.Point(214, 285);
            this.looseLabel.Name = "looseLabel";
            this.looseLabel.Size = new System.Drawing.Size(96, 13);
            this.looseLabel.TabIndex = 42;
            this.looseLabel.Text = "Allow Loose Files:";
            this.looseTooltip.SetToolTip(this.looseLabel,
                "Scan gamedata/load/ for replacement TIM/TMD/etc and use those instead of the disc image. For texture mods.");
            //
            // loosePanel
            //
            this.loosePanel.Controls.Add(this.looseYes);
            this.loosePanel.Controls.Add(this.looseNo);
            this.loosePanel.Location = new System.Drawing.Point(299, 276);
            this.loosePanel.Name = "loosePanel";
            this.loosePanel.Size = new System.Drawing.Size(138, 30);
            this.loosePanel.TabIndex = 45;
            //
            // looseYes
            //
            this.looseYes.Location = new System.Drawing.Point(5, 5);
            this.looseYes.Name = "looseYes";
            this.looseYes.Size = new System.Drawing.Size(49, 24);
            this.looseYes.TabIndex = 43;
            this.looseYes.Text = "Yes";
            this.looseTooltip.SetToolTip(this.looseYes,
                "Scan gamedata/load/ for replacement TIM/TMD/etc and use those instead of the disc image. For texture mods.");
            //
            // looseNo
            //
            this.looseNo.Location = new System.Drawing.Point(59, 5);
            this.looseNo.Name = "looseNo";
            this.looseNo.Size = new System.Drawing.Size(45, 24);
            this.looseNo.TabIndex = 44;
            this.looseNo.Text = "No";
            this.looseTooltip.SetToolTip(this.looseNo,
                "Scan gamedata/load/ for replacement TIM/TMD/etc and use those instead of the disc image. For texture mods.");
            //
            // Form1
            //
            this.ClientSize = new System.Drawing.Size(400, 380);
            this.Controls.Add(this.cullLabel);
            this.Controls.Add(this.chunksLabel);
            this.Controls.Add(this.fullscreenLabel);
            this.Controls.Add(this.vsyncLabel);
            this.Controls.Add(this.levelLabel);
            this.Controls.Add(this.introLabel);
            this.Controls.Add(this.comboResolution);
            this.Controls.Add(this.comboRefresh);
            this.Controls.Add(this.banner);
            this.Controls.Add(this.comboMap);
            this.Controls.Add(this.btnPlay);
            this.Controls.Add(this.resolutionLabel);
            this.Controls.Add(this.refreshLabel);
            this.Controls.Add(this.fullscreenPanel);
            this.Controls.Add(this.vsyncPanel);
            this.Controls.Add(this.cullingPanel);
            this.Controls.Add(this.preloadPanel);
            this.Controls.Add(this.introPanel);
            this.Controls.Add(this.fpsLabel);
            this.Controls.Add(this.comboFps);
            this.Controls.Add(this.filteringLabel);
            this.Controls.Add(this.comboFiltering);
            this.Controls.Add(this.loggingLabel);
            this.Controls.Add(this.loggingPanel);
            this.Controls.Add(this.consoleLabel);
            this.Controls.Add(this.consolePanel);
            this.Controls.Add(this.looseLabel);
            this.Controls.Add(this.loosePanel);
            this.Name = "Form1";
            this.Text = "Silent Hill Launcher";
            this.Load += new System.EventHandler(this.Form1_Load);
            this.fullscreenPanel.ResumeLayout(false);
            this.vsyncPanel.ResumeLayout(false);
            this.cullingPanel.ResumeLayout(false);
            this.preloadPanel.ResumeLayout(false);
            this.introPanel.ResumeLayout(false);
            ((System.ComponentModel.ISupportInitialize)(this.banner)).EndInit();
            this.loggingPanel.ResumeLayout(false);
            this.consolePanel.ResumeLayout(false);
            this.loosePanel.ResumeLayout(false);
            this.ResumeLayout(false);
            this.PerformLayout();

    }

    private Label chunksLabel;
}
