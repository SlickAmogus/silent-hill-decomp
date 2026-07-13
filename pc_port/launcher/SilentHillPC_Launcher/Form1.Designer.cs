using System;
using System.Windows.Forms;

partial class Form1
{
    private System.ComponentModel.IContainer components = null;

    private PictureBox banner;
    private ComboBox comboFullscreen;
    private RadioButton radioVsyncYes;
    private RadioButton radioVsyncNo;
    private Button btnManager;
    private Button btnPlay;
    private Button btnUpdate;
    private Button btnChangelog;
    private Label  lblUpdateStatus;
    private ProgressBar progUpdate;

    private ComboBox comboResolution;
    private RadioButton radioCullingYes;
    private RadioButton radioCullingNo;
    private RadioButton radioPreloadYes;
    private RadioButton radioPreloadNo;
    private ComboBox comboPillarbox;

    private Label cullLabel;
    private Label fullscreenLabel;
    private Label vsyncLabel;
    private Label resolutionLabel;
    private Label refreshLabel;

    private Panel vsyncPanel;
    private Panel cullingPanel;
    private Panel preloadPanel;
    private Panel pillarboxPanel;

    private Label fpsLabel;
    private ComboBox comboFps;

    private Label filteringLabel;
    private ComboBox comboFiltering;

    private Label pgxpLabel;
    private Panel pgxpPanel;
    private RadioButton pgxpYes;
    private RadioButton pgxpNo;

    private Label loggingLabel;
    private Panel loggingPanel;
    private RadioButton loggingYes;
    private RadioButton loggingNo;

    private Label consoleLabel;
    private Panel consolePanel;
    private RadioButton consoleYes;
    private RadioButton consoleNo;
    private Label aaLabel;
    private ComboBox comboAA;
    private Label postLabel;
    private ComboBox comboPost;
    private Label toneLabel;
    private ComboBox comboTone;
    private Label flashLabel;
    private ComboBox comboFlash;
    private Label regionLabel;
    private ComboBox comboRegion;
    private ComboBox comboDisc;
    private Label lblDisc;



    protected override void Dispose(bool disposing)
    {
        if (disposing && (components != null))
            components.Dispose();
        base.Dispose(disposing);
    }

    private void InitializeComponent()
    {
            this.comboFullscreen = new System.Windows.Forms.ComboBox();
            this.vsyncPanel = new System.Windows.Forms.Panel();
            this.radioVsyncYes = new System.Windows.Forms.RadioButton();
            this.radioVsyncNo = new System.Windows.Forms.RadioButton();
            this.cullingPanel = new System.Windows.Forms.Panel();
            this.radioCullingYes = new System.Windows.Forms.RadioButton();
            this.radioCullingNo = new System.Windows.Forms.RadioButton();
            this.preloadPanel = new System.Windows.Forms.Panel();
            this.radioPreloadYes = new System.Windows.Forms.RadioButton();
            this.radioPreloadNo = new System.Windows.Forms.RadioButton();
            this.pillarboxPanel = new System.Windows.Forms.Panel();
            this.comboPillarbox = new System.Windows.Forms.ComboBox();
            this.btnManager = new System.Windows.Forms.Button();
            this.btnPlay = new System.Windows.Forms.Button();
            this.btnUpdate = new System.Windows.Forms.Button();
            this.btnChangelog = new System.Windows.Forms.Button();
            this.lblUpdateStatus = new System.Windows.Forms.Label();
            this.progUpdate = new System.Windows.Forms.ProgressBar();
            this.comboResolution = new System.Windows.Forms.ComboBox();
            this.banner = new System.Windows.Forms.PictureBox();
            this.cullLabel = new System.Windows.Forms.Label();
            this.fullscreenLabel = new System.Windows.Forms.Label();
            this.vsyncLabel = new System.Windows.Forms.Label();
            this.resolutionLabel = new System.Windows.Forms.Label();
            this.refreshLabel = new System.Windows.Forms.Label();
            this.fpsLabel = new System.Windows.Forms.Label();
            this.chunksLabel = new System.Windows.Forms.Label();
            this.comboFps = new System.Windows.Forms.ComboBox();
            this.filteringLabel = new System.Windows.Forms.Label();
            this.comboFiltering = new System.Windows.Forms.ComboBox();
            this.pgxpLabel = new System.Windows.Forms.Label();
            this.pgxpPanel = new System.Windows.Forms.Panel();
            this.pgxpYes = new System.Windows.Forms.RadioButton();
            this.pgxpNo = new System.Windows.Forms.RadioButton();
            this.loggingLabel = new System.Windows.Forms.Label();
            this.loggingPanel = new System.Windows.Forms.Panel();
            this.loggingYes = new System.Windows.Forms.RadioButton();
            this.loggingNo = new System.Windows.Forms.RadioButton();
            this.consoleLabel = new System.Windows.Forms.Label();
            this.consolePanel = new System.Windows.Forms.Panel();
            this.consoleYes = new System.Windows.Forms.RadioButton();
            this.consoleNo = new System.Windows.Forms.RadioButton();
            this.aaLabel = new System.Windows.Forms.Label();
            this.comboAA = new System.Windows.Forms.ComboBox();
            this.postLabel = new System.Windows.Forms.Label();
            this.comboPost = new System.Windows.Forms.ComboBox();
            this.toneLabel = new System.Windows.Forms.Label();
            this.comboTone = new System.Windows.Forms.ComboBox();
            this.flashLabel = new System.Windows.Forms.Label();
            this.comboFlash = new System.Windows.Forms.ComboBox();
            this.regionLabel = new System.Windows.Forms.Label();
            this.comboRegion = new System.Windows.Forms.ComboBox();
            this.comboDisc = new System.Windows.Forms.ComboBox();
            this.lblDisc = new System.Windows.Forms.Label();
            this.btnControls = new System.Windows.Forms.Button();
            this.btnBuildSettings = new System.Windows.Forms.Button();
            this.downloadBuild = new System.Windows.Forms.Button();
            this.btnHelp = new System.Windows.Forms.Button();
            this.btnBug = new System.Windows.Forms.Button();
            this.btnReset = new System.Windows.Forms.Button();
            this.comboRefresh = new System.Windows.Forms.ComboBox();
            this.vsyncPanel.SuspendLayout();
            this.cullingPanel.SuspendLayout();
            this.preloadPanel.SuspendLayout();
            this.pillarboxPanel.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.banner)).BeginInit();
            this.pgxpPanel.SuspendLayout();
            this.loggingPanel.SuspendLayout();
            this.consolePanel.SuspendLayout();
            this.SuspendLayout();
            // 
            // comboFullscreen
            // 
            this.comboFullscreen.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboFullscreen.Items.AddRange(new object[] {
            "Fullscreen",
            "Windowed",
            "Borderless"});
            this.comboFullscreen.Location = new System.Drawing.Point(80, 123);
            this.comboFullscreen.Name = "comboFullscreen";
            this.comboFullscreen.Size = new System.Drawing.Size(120, 21);
            this.comboFullscreen.TabIndex = 16;
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
            this.radioPreloadYes.Location = new System.Drawing.Point(4, 5);
            this.radioPreloadYes.Name = "radioPreloadYes";
            this.radioPreloadYes.Size = new System.Drawing.Size(45, 24);
            this.radioPreloadYes.TabIndex = 4;
            this.radioPreloadYes.Text = "Yes";
            this.radioPreloadYes.CheckedChanged += new System.EventHandler(this.radioPreloadYes_CheckedChanged);
            // 
            // radioPreloadNo
            // 
            this.radioPreloadNo.Location = new System.Drawing.Point(59, 5);
            this.radioPreloadNo.Name = "radioPreloadNo";
            this.radioPreloadNo.Size = new System.Drawing.Size(53, 24);
            this.radioPreloadNo.TabIndex = 5;
            this.radioPreloadNo.Text = "No";
            // 
            // pillarboxPanel
            // 
            this.pillarboxPanel.Controls.Add(this.comboPillarbox);
            this.pillarboxPanel.Location = new System.Drawing.Point(80, 207);
            this.pillarboxPanel.Name = "pillarboxPanel";
            this.pillarboxPanel.Size = new System.Drawing.Size(120, 30);
            this.pillarboxPanel.TabIndex = 21;
            this.pillarboxPanel.Paint += new System.Windows.Forms.PaintEventHandler(this.pillarboxPanel_Paint);
            // 
            // comboPillarbox
            // 
            this.comboPillarbox.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboPillarbox.Items.AddRange(new object[] {
            "Yes",
            "No",
            "Menus Only"});
            this.comboPillarbox.Location = new System.Drawing.Point(0, 4);
            this.comboPillarbox.Name = "comboPillarbox";
            this.comboPillarbox.Size = new System.Drawing.Size(120, 21);
            this.comboPillarbox.TabIndex = 4;
            //
            // btnManager
            //
            this.btnManager.BackgroundImage = global::SilentHillPC_Launcher.Properties.Resources.manager;
            this.btnManager.BackgroundImageLayout = System.Windows.Forms.ImageLayout.Zoom;
            this.btnManager.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
            this.btnManager.FlatAppearance.BorderSize = 0;
            this.btnManager.Location = new System.Drawing.Point(216, 271);
            this.btnManager.Name = "btnManager";
            this.btnManager.Size = new System.Drawing.Size(192, 48);
            this.btnManager.TabIndex = 11;
            this.btnManager.UseVisualStyleBackColor = true;
            this.btnManager.Click += new System.EventHandler(this.btnManager_Click);
            this.btnManager.MouseDown += new System.Windows.Forms.MouseEventHandler(this.btnManager_MouseDown);
            this.btnManager.MouseUp += new System.Windows.Forms.MouseEventHandler(this.btnManager_MouseUp);
            this.btnManager.MouseLeave += new System.EventHandler(this.btnManager_MouseLeave);
            //
            // btnPlay
            //
            this.btnPlay.Location = new System.Drawing.Point(314, 388);
            this.btnPlay.Name = "btnPlay";
            this.btnPlay.Size = new System.Drawing.Size(97, 23);
            this.btnPlay.TabIndex = 12;
            this.btnPlay.Text = "Play";
            this.btnPlay.Click += new System.EventHandler(this.btnPlay_Click);
            // 
            // btnUpdate
            // 
            this.btnUpdate.Location = new System.Drawing.Point(206, 331);
            this.btnUpdate.Name = "btnUpdate";
            this.btnUpdate.Size = new System.Drawing.Size(104, 23);
            this.btnUpdate.TabIndex = 13;
            this.btnUpdate.Text = "Check for Updates";
            this.btnUpdate.UseVisualStyleBackColor = true;
            this.btnUpdate.Click += new System.EventHandler(this.btnUpdate_Click);
            // 
            // btnChangelog
            // 
            this.btnChangelog.Location = new System.Drawing.Point(206, 360);
            this.btnChangelog.Name = "btnChangelog";
            this.btnChangelog.Size = new System.Drawing.Size(104, 23);
            this.btnChangelog.TabIndex = 16;
            this.btnChangelog.Text = "Changelog";
            this.btnChangelog.UseVisualStyleBackColor = true;
            this.btnChangelog.Click += new System.EventHandler(this.btnChangelog_Click);
            // 
            // lblUpdateStatus
            // 
            this.lblUpdateStatus.Location = new System.Drawing.Point(209, 480);
            this.lblUpdateStatus.Name = "lblUpdateStatus";
            this.lblUpdateStatus.Size = new System.Drawing.Size(206, 15);
            this.lblUpdateStatus.TabIndex = 14;
            // 
            // progUpdate
            // 
            this.progUpdate.Location = new System.Drawing.Point(206, 456);
            this.progUpdate.Name = "progUpdate";
            this.progUpdate.Size = new System.Drawing.Size(205, 16);
            this.progUpdate.TabIndex = 15;
            this.progUpdate.Visible = false;
            this.progUpdate.Click += new System.EventHandler(this.progUpdate_Click);
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
            // banner
            // 
            this.banner.BackColor = System.Drawing.Color.DarkSlateGray;
            this.banner.Cursor = System.Windows.Forms.Cursors.Hand;
            this.banner.Dock = System.Windows.Forms.DockStyle.Top;
            this.banner.Image = global::SilentHillPC_Launcher.Properties.Resources.launcher;
            this.banner.Location = new System.Drawing.Point(0, 0);
            this.banner.Name = "banner";
            this.banner.Size = new System.Drawing.Size(420, 111);
            this.banner.SizeMode = System.Windows.Forms.PictureBoxSizeMode.StretchImage;
            this.banner.TabIndex = 6;
            this.banner.TabStop = false;
            this.banner.Click += new System.EventHandler(this.banner_Click);
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
            this.fullscreenLabel.Size = new System.Drawing.Size(44, 13);
            this.fullscreenLabel.TabIndex = 2;
            this.fullscreenLabel.Text = "Display:";
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
            this.refreshLabel.Size = new System.Drawing.Size(63, 13);
            this.refreshLabel.TabIndex = 14;
            this.refreshLabel.Text = "Pillarboxing:";
            //
            // fpsLabel
            // 
            this.fpsLabel.AutoSize = true;
            this.fpsLabel.Location = new System.Drawing.Point(216, 247);
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
            this.comboFps.Location = new System.Drawing.Point(288, 243);
            this.comboFps.Name = "comboFps";
            this.comboFps.Size = new System.Drawing.Size(120, 21);
            this.comboFps.TabIndex = 31;
            // 
            // filteringLabel
            // 
            this.filteringLabel.AutoSize = true;
            this.filteringLabel.Location = new System.Drawing.Point(8, 246);
            this.filteringLabel.Name = "filteringLabel";
            this.filteringLabel.Size = new System.Drawing.Size(46, 13);
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
            this.comboFiltering.Location = new System.Drawing.Point(80, 242);
            this.comboFiltering.Name = "comboFiltering";
            this.comboFiltering.Size = new System.Drawing.Size(120, 21);
            this.comboFiltering.TabIndex = 41;
            // 
            // pgxpLabel
            // 
            this.pgxpLabel.AutoSize = true;
            this.pgxpLabel.Location = new System.Drawing.Point(8, 275);
            this.pgxpLabel.Name = "pgxpLabel";
            this.pgxpLabel.Size = new System.Drawing.Size(61, 13);
            this.pgxpLabel.TabIndex = 46;
            this.pgxpLabel.Text = "Use PGXP:";
            // 
            // pgxpPanel
            // 
            this.pgxpPanel.Controls.Add(this.pgxpYes);
            this.pgxpPanel.Controls.Add(this.pgxpNo);
            this.pgxpPanel.Location = new System.Drawing.Point(80, 266);
            this.pgxpPanel.Name = "pgxpPanel";
            this.pgxpPanel.Size = new System.Drawing.Size(120, 30);
            this.pgxpPanel.TabIndex = 49;
            // 
            // pgxpYes
            // 
            this.pgxpYes.Location = new System.Drawing.Point(5, 5);
            this.pgxpYes.Name = "pgxpYes";
            this.pgxpYes.Size = new System.Drawing.Size(49, 24);
            this.pgxpYes.TabIndex = 47;
            this.pgxpYes.Text = "Yes";
            this.pgxpYes.CheckedChanged += new System.EventHandler(this.pgxpYes_CheckedChanged);
            // 
            // pgxpNo
            // 
            this.pgxpNo.Location = new System.Drawing.Point(59, 5);
            this.pgxpNo.Name = "pgxpNo";
            this.pgxpNo.Size = new System.Drawing.Size(45, 24);
            this.pgxpNo.TabIndex = 48;
            this.pgxpNo.Text = "No";
            // 
            // loggingLabel
            // 
            this.loggingLabel.AutoSize = true;
            this.loggingLabel.Location = new System.Drawing.Point(214, 187);
            this.loggingLabel.Name = "loggingLabel";
            this.loggingLabel.Size = new System.Drawing.Size(84, 13);
            this.loggingLabel.TabIndex = 32;
            this.loggingLabel.Text = "Enable Logging:";
            // 
            // loggingPanel
            // 
            this.loggingPanel.Controls.Add(this.loggingYes);
            this.loggingPanel.Controls.Add(this.loggingNo);
            this.loggingPanel.Location = new System.Drawing.Point(299, 178);
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
            this.consoleLabel.Location = new System.Drawing.Point(214, 217);
            this.consoleLabel.Name = "consoleLabel";
            this.consoleLabel.Size = new System.Drawing.Size(89, 13);
            this.consoleLabel.TabIndex = 36;
            this.consoleLabel.Text = "External Console:";
            this.consoleLabel.Click += new System.EventHandler(this.consoleLabel_Click);
            // 
            // consolePanel
            // 
            this.consolePanel.Controls.Add(this.consoleYes);
            this.consolePanel.Controls.Add(this.consoleNo);
            this.consolePanel.Location = new System.Drawing.Point(300, 208);
            this.consolePanel.Name = "consolePanel";
            this.consolePanel.Size = new System.Drawing.Size(120, 30);
            this.consolePanel.TabIndex = 56;
            // 
            // consoleYes
            // 
            this.consoleYes.Location = new System.Drawing.Point(4, 5);
            this.consoleYes.Name = "consoleYes";
            this.consoleYes.Size = new System.Drawing.Size(49, 24);
            this.consoleYes.TabIndex = 0;
            this.consoleYes.Text = "Yes";
            // 
            // consoleNo
            // 
            this.consoleNo.Location = new System.Drawing.Point(58, 5);
            this.consoleNo.Name = "consoleNo";
            this.consoleNo.Size = new System.Drawing.Size(45, 24);
            this.consoleNo.TabIndex = 1;
            this.consoleNo.Text = "No";
            // 
            // aaLabel
            // 
            this.aaLabel.AutoSize = true;
            this.aaLabel.Location = new System.Drawing.Point(8, 307);
            this.aaLabel.Name = "aaLabel";
            this.aaLabel.Size = new System.Drawing.Size(63, 13);
            this.aaLabel.TabIndex = 57;
            this.aaLabel.Text = "Antialiasing:";
            // 
            // comboAA
            // 
            this.comboAA.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboAA.Items.AddRange(new object[] {
            "Off",
            "2x",
            "4x",
            "8x"});
            this.comboAA.Location = new System.Drawing.Point(80, 303);
            this.comboAA.Name = "comboAA";
            this.comboAA.Size = new System.Drawing.Size(120, 21);
            this.comboAA.TabIndex = 58;
            // 
            // postLabel
            // 
            this.postLabel.AutoSize = true;
            this.postLabel.Location = new System.Drawing.Point(8, 337);
            this.postLabel.Name = "postLabel";
            this.postLabel.Size = new System.Drawing.Size(62, 13);
            this.postLabel.TabIndex = 59;
            this.postLabel.Text = "Post Effect:";
            // 
            // comboPost
            // 
            this.comboPost.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboPost.Items.AddRange(new object[] {
            "Off",
            "CRT",
            "Scanlines",
            "Vignette",
            "Color Grade",
            "Film Grain",
            "Sharpen",
            "PSX Retro",
            "Cinematic"});
            this.comboPost.Location = new System.Drawing.Point(80, 333);
            this.comboPost.Name = "comboPost";
            this.comboPost.Size = new System.Drawing.Size(120, 21);
            this.comboPost.TabIndex = 59;
            // 
            // toneLabel
            // 
            this.toneLabel.AutoSize = true;
            this.toneLabel.Location = new System.Drawing.Point(8, 367);
            this.toneLabel.Name = "toneLabel";
            this.toneLabel.Size = new System.Drawing.Size(59, 13);
            this.toneLabel.TabIndex = 60;
            this.toneLabel.Text = "Tone Map:";
            // 
            // comboTone
            // 
            this.comboTone.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboTone.Items.AddRange(new object[] {
            "Off",
            "Reinhard",
            "ACES",
            "Filmic"});
            this.comboTone.Location = new System.Drawing.Point(80, 363);
            this.comboTone.Name = "comboTone";
            this.comboTone.Size = new System.Drawing.Size(120, 21);
            this.comboTone.TabIndex = 60;
            // 
            // flashLabel
            // 
            this.flashLabel.AutoSize = true;
            this.flashLabel.Location = new System.Drawing.Point(8, 397);
            this.flashLabel.Name = "flashLabel";
            this.flashLabel.Size = new System.Drawing.Size(54, 13);
            this.flashLabel.TabIndex = 61;
            this.flashLabel.Text = "Flashlight:";
            // 
            // comboFlash
            // 
            this.comboFlash.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboFlash.Items.AddRange(new object[] {
            "Classic",
            "Classic + Shadows",
            "Modern",
            "Modern + Shadows"});
            this.comboFlash.Location = new System.Drawing.Point(80, 393);
            this.comboFlash.Name = "comboFlash";
            this.comboFlash.Size = new System.Drawing.Size(120, 21);
            this.comboFlash.TabIndex = 61;
            //
            // regionLabel
            //
            this.regionLabel.AutoSize = true;
            this.regionLabel.Location = new System.Drawing.Point(8, 427);
            this.regionLabel.Name = "regionLabel";
            this.regionLabel.Size = new System.Drawing.Size(44, 13);
            this.regionLabel.TabIndex = 62;
            this.regionLabel.Text = "Region:";
            //
            // comboRegion
            //
            this.comboRegion.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboRegion.Location = new System.Drawing.Point(80, 423);
            this.comboRegion.Name = "comboRegion";
            this.comboRegion.Size = new System.Drawing.Size(120, 21);
            this.comboRegion.TabIndex = 63;
            this.comboRegion.SelectedIndexChanged += new System.EventHandler(this.comboRegion_SelectedIndexChanged);
            //
            // comboDisc
            //
            this.comboDisc.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboDisc.Location = new System.Drawing.Point(214, 420);
            this.comboDisc.Name = "comboDisc";
            this.comboDisc.Size = new System.Drawing.Size(198, 21);
            this.comboDisc.DropDownWidth = 320;
            this.comboDisc.TabIndex = 64;
            this.comboDisc.SelectedIndexChanged += new System.EventHandler(this.comboDisc_SelectedIndexChanged);
            //
            // lblDisc
            //
            this.lblDisc.Location = new System.Drawing.Point(214, 443);
            this.lblDisc.Name = "lblDisc";
            this.lblDisc.Size = new System.Drawing.Size(198, 13);
            this.lblDisc.TabIndex = 65;
            //
            // btnControls
            //
            this.btnControls.Location = new System.Drawing.Point(315, 331);
            this.btnControls.Name = "btnControls";
            this.btnControls.Size = new System.Drawing.Size(97, 23);
            this.btnControls.TabIndex = 50;
            this.btnControls.Text = "Controls";
            this.btnControls.UseVisualStyleBackColor = true;
            this.btnControls.Click += new System.EventHandler(this.btnControls_Click);
            // 
            // btnBuildSettings
            // 
            this.btnBuildSettings.Location = new System.Drawing.Point(314, 360);
            this.btnBuildSettings.Name = "btnBuildSettings";
            this.btnBuildSettings.Size = new System.Drawing.Size(98, 23);
            this.btnBuildSettings.TabIndex = 51;
            this.btnBuildSettings.Text = "Build Settings";
            this.btnBuildSettings.UseVisualStyleBackColor = true;
            this.btnBuildSettings.Click += new System.EventHandler(this.btnBuildSettings_Click);
            // 
            // downloadBuild
            // 
            this.downloadBuild.Location = new System.Drawing.Point(205, 388);
            this.downloadBuild.Name = "downloadBuild";
            this.downloadBuild.Size = new System.Drawing.Size(104, 23);
            this.downloadBuild.TabIndex = 52;
            this.downloadBuild.Text = "Download Build";
            this.downloadBuild.UseVisualStyleBackColor = true;
            this.downloadBuild.Click += new System.EventHandler(this.downloadBuild_Click);
            // 
            // btnHelp
            // 
            this.btnHelp.Location = new System.Drawing.Point(11, 463);
            this.btnHelp.Name = "btnHelp";
            this.btnHelp.Size = new System.Drawing.Size(39, 23);
            this.btnHelp.TabIndex = 53;
            this.btnHelp.Text = "Help";
            this.btnHelp.UseVisualStyleBackColor = true;
            this.btnHelp.Click += new System.EventHandler(this.button1_Click);
            // 
            // btnBug
            // 
            this.btnBug.Location = new System.Drawing.Point(113, 463);
            this.btnBug.Name = "btnBug";
            this.btnBug.Size = new System.Drawing.Size(84, 23);
            this.btnBug.TabIndex = 54;
            this.btnBug.Text = "Report Bug";
            this.btnBug.UseVisualStyleBackColor = true;
            this.btnBug.Click += new System.EventHandler(this.button2_Click);
            // 
            // btnReset
            // 
            this.btnReset.Location = new System.Drawing.Point(55, 463);
            this.btnReset.Name = "btnReset";
            this.btnReset.Size = new System.Drawing.Size(53, 23);
            this.btnReset.TabIndex = 55;
            this.btnReset.Text = "Reset";
            this.btnReset.UseVisualStyleBackColor = true;
            this.btnReset.Click += new System.EventHandler(this.button1_Click_1);
            // 
            // comboRefresh
            // 
            this.comboRefresh.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.comboRefresh.Location = new System.Drawing.Point(258, 269);
            this.comboRefresh.Name = "comboRefresh";
            this.comboRefresh.Size = new System.Drawing.Size(120, 21);
            this.comboRefresh.TabIndex = 1;
            this.comboRefresh.Visible = false;
            this.comboRefresh.SelectedIndexChanged += new System.EventHandler(this.comboRefresh_SelectedIndexChanged);
            // 
            // Form1
            // 
            this.ClientSize = new System.Drawing.Size(420, 500);
            this.Controls.Add(this.regionLabel);
            this.Controls.Add(this.comboRegion);
            this.Controls.Add(this.comboDisc);
            this.Controls.Add(this.lblDisc);
            this.Controls.Add(this.btnReset);
            this.Controls.Add(this.btnBug);
            this.Controls.Add(this.btnHelp);
            this.Controls.Add(this.downloadBuild);
            this.Controls.Add(this.btnBuildSettings);
            this.Controls.Add(this.btnControls);
            this.Controls.Add(this.cullLabel);
            this.Controls.Add(this.chunksLabel);
            this.Controls.Add(this.fullscreenLabel);
            this.Controls.Add(this.vsyncLabel);
            this.Controls.Add(this.comboResolution);
            this.Controls.Add(this.comboRefresh);
            this.Controls.Add(this.pillarboxPanel);
            this.Controls.Add(this.banner);
            this.Controls.Add(this.btnManager);
            this.Controls.Add(this.btnPlay);
            this.Controls.Add(this.btnUpdate);
            this.Controls.Add(this.btnChangelog);
            this.Controls.Add(this.lblUpdateStatus);
            this.Controls.Add(this.progUpdate);
            this.Controls.Add(this.resolutionLabel);
            this.Controls.Add(this.refreshLabel);
            this.Controls.Add(this.comboFullscreen);
            this.Controls.Add(this.vsyncPanel);
            this.Controls.Add(this.cullingPanel);
            this.Controls.Add(this.preloadPanel);
            this.Controls.Add(this.fpsLabel);
            this.Controls.Add(this.comboFps);
            this.Controls.Add(this.filteringLabel);
            this.Controls.Add(this.comboFiltering);
            this.Controls.Add(this.loggingLabel);
            this.Controls.Add(this.loggingPanel);
            this.Controls.Add(this.consoleLabel);
            this.Controls.Add(this.consolePanel);
            this.Controls.Add(this.aaLabel);
            this.Controls.Add(this.comboAA);
            this.Controls.Add(this.postLabel);
            this.Controls.Add(this.comboPost);
            this.Controls.Add(this.toneLabel);
            this.Controls.Add(this.comboTone);
            this.Controls.Add(this.flashLabel);
            this.Controls.Add(this.comboFlash);
            this.Controls.Add(this.pgxpLabel);
            this.Controls.Add(this.pgxpPanel);
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedSingle;
            this.MaximizeBox = false;
            this.Name = "Form1";
            this.Text = "Silent Hill Launcher";
            this.Load += new System.EventHandler(this.Form1_Load);
            this.vsyncPanel.ResumeLayout(false);
            this.cullingPanel.ResumeLayout(false);
            this.preloadPanel.ResumeLayout(false);
            this.pillarboxPanel.ResumeLayout(false);
            ((System.ComponentModel.ISupportInitialize)(this.banner)).EndInit();
            this.pgxpPanel.ResumeLayout(false);
            this.loggingPanel.ResumeLayout(false);
            this.consolePanel.ResumeLayout(false);
            this.ResumeLayout(false);
            this.PerformLayout();

    }

    private Label chunksLabel;
    private Button btnControls;
    private Button btnBuildSettings;
    private Button downloadBuild;
    private Button btnHelp;
    private Button btnBug;
    private Button btnReset;
    private ComboBox comboRefresh;
}
