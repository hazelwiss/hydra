#include "mainwindow.hxx"
#include "aboutwindow.hxx"
#include "qthelper.hxx"
#include "scripteditor.hxx"
#include "settingswindow.hxx"
#include "shadereditor.hxx"
#include "terminalwindow.hxx"
#include <error_factory.hxx>
#include <fstream>
#include <iostream>
#include <json.hpp>
#include <log.hxx>
#include <QApplication>
#include <QClipboard>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTimer>
#include <settings.hxx>
#include <sol/sol.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.hxx>

void hungry_for_more(ma_device* device, void* out, const void*, ma_uint32 frames)
{
    MainWindow* window = static_cast<MainWindow*>(device->pUserData);
    std::unique_lock<std::mutex> lock(window->audio_mutex_);
    if (window->queued_audio_.empty())
    {
        return;
    }
    frames = std::min(frames * 2, static_cast<ma_uint32>(window->queued_audio_.size()));
    std::memcpy(out, window->queued_audio_.data(), frames * sizeof(int16_t));
    window->queued_audio_.erase(window->queued_audio_.begin(),
                                window->queued_audio_.begin() + frames);
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    auto settings_path = hydra::UiCommon::GetSavePath() + "settings.json";
    Settings::Open(settings_path);

    QWidget* widget = new QWidget;
    setCentralWidget(widget);

    QVBoxLayout* layout = new QVBoxLayout;
    layout->setContentsMargins(5, 5, 5, 5);
    screen_ = new ScreenWidget(this);
    screen_->SetMouseMoveCallback([this](QMouseEvent* event) { on_mouse_move(event); });
    screen_->setMouseTracking(true);
    layout->addWidget(screen_);
    widget->setLayout(layout);
    create_actions();
    create_menus();

    QString message = tr("Welcome to hydra!");
    statusBar()->showMessage(message);
    setMinimumSize(160, 160);
    resize(640, 480);
    setWindowTitle("hydra");
    setWindowIcon(QIcon(":/images/hydra.png"));

    emulator_timer_ = new QTimer(this);
    connect(emulator_timer_, SIGNAL(timeout()), this, SLOT(emulator_frame()));

    TerminalWindow::Init();
    Logger::HookCallback("Fatal", [this](const std::string& fatal_msg) {
        printf("Fatal: %s\n", fatal_msg.c_str());
        exit(1);
    });

    initialize_emulator_data();
    initialize_audio();
    enable_emulation_actions(false);

    widget->setStyleSheet(R"(
        background-repeat: no-repeat;
        background-position: center;
        background-image: url(:/images/hydra.png);
    )");
}

MainWindow::~MainWindow()
{
    ma_device_uninit(&sound_device_);
    stop_emulator();
}

void MainWindow::OpenFile(const std::string& file)
{
    open_file_impl(file);
}

void MainWindow::initialize_audio()
{
    ma_context context;
    ma_context_config context_config = ma_context_config_init();
    context_config.threadPriority = ma_thread_priority_realtime;
    if (ma_context_init(NULL, 0, &context_config, &context) != MA_SUCCESS)
    {
        Logger::Fatal("Failed to initialize audio context");
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.periodSizeInFrames = 48000 / 60;
    config.dataCallback = hungry_for_more;
    config.pUserData = this;
    if (ma_device_init(NULL, &config, &sound_device_) != MA_SUCCESS)
    {
        Logger::Fatal("Failed to open audio device");
    }
    ma_device_start(&sound_device_);

    std::string volume_str = Settings::Get("master_volume");
    if (!volume_str.empty())
    {
        int volume = std::stoi(volume_str);
        float volume_f = static_cast<float>(volume) / 100.0f;
        ma_device_set_master_volume(&sound_device_, volume_f);
    }
    else
    {
        ma_device_set_master_volume(&sound_device_, 1.0f);
    }
}

void MainWindow::create_actions()
{
    open_act_ = new QAction(tr("&Open ROM"), this);
    open_act_->setShortcuts(QKeySequence::Open);
    open_act_->setStatusTip(tr("Open a ROM"));
    open_act_->setIcon(QIcon(":/images/open.png"));
    connect(open_act_, &QAction::triggered, this, &MainWindow::open_file);
    settings_act_ = new QAction(tr("&Settings"), this);
    settings_act_->setShortcut(Qt::CTRL | Qt::Key_Comma);
    settings_act_->setStatusTip(tr("Emulator settings"));
    connect(settings_act_, &QAction::triggered, this, &MainWindow::open_settings);
    close_act_ = new QAction(tr("&Exit"), this);
    close_act_->setShortcut(QKeySequence::Close);
    close_act_->setStatusTip(tr("Exit hydra"));
    connect(close_act_, &QAction::triggered, this, &MainWindow::close);
    pause_act_ = new QAction(tr("&Pause"), this);
    pause_act_->setShortcut(Qt::CTRL | Qt::Key_P);
    pause_act_->setCheckable(true);
    pause_act_->setStatusTip(tr("Pause emulation"));
    connect(pause_act_, &QAction::triggered, this, &MainWindow::pause_emulator);
    stop_act_ = new QAction(tr("&Stop"), this);
    stop_act_->setShortcut(Qt::CTRL | Qt::Key_Q);
    stop_act_->setStatusTip(tr("Stop emulation immediately"));
    connect(stop_act_, &QAction::triggered, this, &MainWindow::stop_emulator);
    mute_act_ = new QAction(tr("&Mute"), this);
    mute_act_->setShortcut(Qt::CTRL | Qt::Key_M);
    mute_act_->setCheckable(true);
    mute_act_->setStatusTip(tr("Mute audio"));
    connect(mute_act_, &QAction::triggered, this, [this]() {
        if (mute_act_->isChecked())
        {
            ma_device_set_master_volume(&sound_device_, 0.0f);
        }
        else
        {
            std::string volume_str = Settings::Get("master_volume");
            if (!volume_str.empty())
            {
                int volume = std::stoi(volume_str);
                float volume_f = static_cast<float>(volume) / 100.0f;
                ma_device_set_master_volume(&sound_device_, volume_f);
            }
            else
            {
                ma_device_set_master_volume(&sound_device_, 1.0f);
            }
        }
    });
    reset_act_ = new QAction(tr("&Reset"), this);
    reset_act_->setShortcut(Qt::CTRL | Qt::Key_R);
    reset_act_->setStatusTip(tr("Reset emulator"));
    connect(reset_act_, &QAction::triggered, this, &MainWindow::reset_emulator);
    screenshot_act_ = new QAction(tr("S&creenshot"), this);
    screenshot_act_->setShortcut(Qt::Key_F12);
    screenshot_act_->setStatusTip(tr("Take a screenshot (check settings for save path)"));
    connect(screenshot_act_, &QAction::triggered, this, &MainWindow::screenshot);
    about_act_ = new QAction(tr("&About"), this);
    about_act_->setShortcut(QKeySequence::HelpContents);
    about_act_->setStatusTip(tr("Show about dialog"));
    connect(about_act_, &QAction::triggered, this, &MainWindow::open_about);
    shaders_act_ = new QAction(tr("&Shaders"), this);
    shaders_act_->setShortcut(Qt::Key_F11);
    shaders_act_->setStatusTip("Open the shader editor");
    shaders_act_->setIcon(QIcon(":/images/shaders.png"));
    connect(shaders_act_, &QAction::triggered, this, &MainWindow::open_shaders);
    scripts_act_ = new QAction(tr("S&cripts"), this);
    scripts_act_->setShortcut(Qt::Key_F10);
    scripts_act_->setStatusTip("Open the script editor");
    scripts_act_->setIcon(QIcon(":/images/scripts.png"));
    connect(scripts_act_, &QAction::triggered, this, &MainWindow::open_scripts);
    terminal_act_ = new QAction(tr("&Terminal"), this);
    terminal_act_->setShortcut(Qt::Key_F9);
    terminal_act_->setStatusTip("Open the terminal");
    terminal_act_->setIcon(QIcon(":/images/terminal.png"));
    connect(terminal_act_, &QAction::triggered, this, &MainWindow::open_terminal);
    recent_act_ = new QAction(tr("&Recent files"), this);
    for (int i = 0; i < 10; i++)
    {
        std::string path = Settings::Get("recent_" + std::to_string(i));
        if (!path.empty())
        {
            recent_files_.push_back(path);
        }
        else
        {
            break;
        }
    }
    if (recent_files_.size() == 0)
    {
        QMenu* empty_menu = new QMenu;
        QAction* empty_act = new QAction(tr("No recent files"));
        empty_act->setEnabled(false);
        empty_menu->addAction(empty_act);
        recent_act_->setMenu(empty_menu);
    }
    else
    {
        update_recent_files();
    }
}

void MainWindow::create_menus()
{
    file_menu_ = menuBar()->addMenu(tr("&File"));
    file_menu_->addAction(open_act_);
    file_menu_->addAction(recent_act_);
    file_menu_->addSeparator();
    file_menu_->addAction(screenshot_act_);
    file_menu_->addSeparator();
    file_menu_->addAction(settings_act_);
    file_menu_->addSeparator();
    file_menu_->addAction(close_act_);
    emulation_menu_ = menuBar()->addMenu(tr("&Emulation"));
    emulation_menu_->addAction(pause_act_);
    emulation_menu_->addAction(reset_act_);
    emulation_menu_->addAction(stop_act_);
    emulation_menu_->addSeparator();
    emulation_menu_->addAction(mute_act_);
    tools_menu_ = menuBar()->addMenu(tr("&Tools"));
    tools_menu_->addAction(terminal_act_);
    tools_menu_->addAction(scripts_act_);
    tools_menu_->addAction(shaders_act_);
    help_menu_ = menuBar()->addMenu(tr("&Help"));
    help_menu_->addAction(about_act_);
}

// Shitty input but cba
void MainWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
        case Qt::Key_Right:
        {
            input_state_[hydra::InputButton::AnalogHorizontal_0] = 127;
            break;
        }
        case Qt::Key_Left:
        {
            input_state_[hydra::InputButton::AnalogHorizontal_0] = -127;
            break;
        }
        case Qt::Key_Up:
        {
            input_state_[hydra::InputButton::AnalogVertical_0] = 127;
            break;
        }
        case Qt::Key_Down:
        {
            input_state_[hydra::InputButton::AnalogVertical_0] = -127;
            break;
        }
        case Qt::Key_Z:
        {
            input_state_[hydra::InputButton::A] = 1;
            break;
        }
        case Qt::Key_X:
        {
            input_state_[hydra::InputButton::B] = 1;
            break;
        }
        case Qt::Key_C:
        {
            input_state_[hydra::InputButton::Z] = 1;
            break;
        }
        case Qt::Key_Return:
        {
            input_state_[hydra::InputButton::Start] = 1;
            break;
        }
    }
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    switch (event->key())
    {
        case Qt::Key_Left:
        case Qt::Key_Right:
        {
            input_state_[hydra::InputButton::AnalogHorizontal_0] = 0;
            break;
        }
        case Qt::Key_Up:
        case Qt::Key_Down:
        {
            input_state_[hydra::InputButton::AnalogVertical_0] = 0;
            break;
        }
        case Qt::Key_Z:
        {
            input_state_[hydra::InputButton::A] = 0;
            break;
        }
        case Qt::Key_X:
        {
            input_state_[hydra::InputButton::B] = 0;
            break;
        }
        case Qt::Key_C:
        {
            input_state_[hydra::InputButton::Z] = 0;
            break;
        }
        case Qt::Key_Return:
        {
            input_state_[hydra::InputButton::Start] = 0;
            break;
        }
    }
}

void MainWindow::on_mouse_move(QMouseEvent* event) {}

void MainWindow::open_file()
{
    static QString extensions;
    if (extensions.isEmpty())
    {
        QString indep;
        extensions = "All supported types (";
        for (int i = 0; i < EmuTypeSize; i++)
        {
            const auto& emulator_data = hydra::UiCommon::EmulatorData[i];
            indep += emulator_data.Name.c_str();
            indep += " (";
            for (const auto& str : emulator_data.Extensions)
            {
                extensions += "*";
                extensions += str.c_str();
                extensions += " ";
                indep += "*";
                indep += str.c_str();
                indep += " ";
            }
            indep += ");;";
        }
        extensions += ");;";
        extensions += indep;
    }
    std::string last_path = Settings::Get("last_path");
    std::string path =
        QFileDialog::getOpenFileName(this, tr("Open ROM"), QString::fromStdString(last_path),
                                     extensions, nullptr, QFileDialog::ReadOnly)
            .toStdString();
    if (path.empty())
    {
        return;
    }
    qt_may_throw(std::bind(&MainWindow::open_file_impl, this, path));
}

void MainWindow::open_file_impl(const std::string& path)
{
    std::unique_lock<std::mutex> elock(emulator_mutex_);
    emulator_timer_->stop();
    std::filesystem::path pathfs(path);

    if (!std::filesystem::is_regular_file(pathfs))
    {
        Logger::Warn("Failed to open file: {}", path);
        return;
    }

    stop_emulator();
    Settings::Set("last_path", pathfs.parent_path().string());
    Logger::ClearWarnings();
    auto type = hydra::UiCommon::GetEmulatorType(path);
    emulator_type_ = type;
    emulator_ = hydra::UiCommon::Create(type);
    if (!emulator_)
        throw ErrorFactory::generate_exception(__func__, __LINE__, "Failed to create emulator");
    emulator_->SetVideoCallback(
        std::bind(&MainWindow::video_callback, this, std::placeholders::_1));
    emulator_->SetAudioCallback(
        std::bind(&MainWindow::audio_callback, this, std::placeholders::_1));
    emulator_->SetPollInputCallback(std::bind(&MainWindow::poll_input_callback, this));
    emulator_->SetReadInputCallback(
        std::bind(&MainWindow::read_input_callback, this, std::placeholders::_1));
    if (!emulator_->LoadFile("rom", path))
        throw ErrorFactory::generate_exception(__func__, __LINE__, "Failed to open ROM");
    enable_emulation_actions(true);
    add_recent(path);

    if (!paused_)
    {
        emulator_timer_->start();
    }
}

void MainWindow::open_settings()
{
    qt_may_throw([this]() {
        if (!settings_open_)
        {
            using namespace std::placeholders;
            new SettingsWindow(settings_open_, std::bind(&MainWindow::set_volume, this, _1), this);
        }
    });
}

void MainWindow::open_shaders()
{
    qt_may_throw([this]() {
        if (!shaders_open_)
        {
            using namespace std::placeholders;
            std::function<void(QString*, QString*)> callback =
                std::bind(&ScreenWidget::ResetProgram, screen_, _1, _2);
            new ShaderEditor(shaders_open_, callback, this);
        }
    });
}

void MainWindow::open_scripts()
{
    qt_may_throw([this]() {
        if (!scripts_open_)
        {
            using namespace std::placeholders;
            new ScriptEditor(scripts_open_, std::bind(&MainWindow::run_script, this, _1, _2), this);
        }
    });
}

void MainWindow::open_terminal()
{
    qt_may_throw([this]() {
        if (!terminal_open_)
        {
            new TerminalWindow(terminal_open_, this);
        }
    });
}

void MainWindow::run_script(const std::string& script, bool safe_mode)
{
    static bool initialized = false;
    static sol::state lua;
    static auto environment = sol::environment(lua, sol::create);
    if (!initialized)
    {
        lua.open_libraries();
        const std::vector<std::string> whitelisted = {
            "assert", "error",    "ipairs",   "next", "pairs",  "pcall",  "print",
            "select", "tonumber", "tostring", "type", "unpack", "xpcall",
        };

        for (const auto& name : whitelisted)
        {
            environment[name] = lua[name];
        }

        std::vector<std::string> libraries = {"coroutine", "string", "table", "math"};

        for (const auto& library : libraries)
        {
            sol::table copy(lua, sol::create);
            for (auto [name, func] : lua[library].tbl)
            {
                copy[name] = func;
            }
            environment[library] = copy;
        }

        sol::table os(lua, sol::create);
        os["clock"] = lua["os"]["clock"];
        os["date"] = lua["os"]["date"];
        os["difftime"] = lua["os"]["difftime"];
        os["time"] = lua["os"]["time"];
        environment["os"] = os;
        initialized = true;
    }

    qt_may_throw([this, &script, &safe_mode]() {
        if (safe_mode)
        {
            lua.script(script, environment);
        }
        else
        {
            lua.script(script);
        }
    });
}

void MainWindow::screenshot()
{
    std::unique_lock<std::mutex> elock(emulator_mutex_);
    std::filesystem::path screenshot_path = Settings::Get("screenshot_path");
    if (screenshot_path.empty())
    {
        screenshot_path = std::filesystem::current_path();
    }

    int screenshot_index = 0;
    std::string screenshot_name = "hydra_screenshot";
    std::string screenshot_extension = ".png";
    while (std::filesystem::exists(screenshot_path / (screenshot_name + screenshot_extension)))
    {
        screenshot_index++;
        screenshot_name = "hydra_screenshot_" + std::to_string(screenshot_index);
    }

    std::filesystem::path screenshot_full_path =
        screenshot_path / (screenshot_name + screenshot_extension);
    stbi_write_png(screenshot_full_path.string().c_str(), video_info_.width, video_info_.height, 4,
                   video_info_.data.data(), video_info_.width * 4);
}

void MainWindow::set_volume(int volume)
{
    ma_device_set_master_volume(&sound_device_, volume / 100.0f);
}

void MainWindow::open_about()
{
    qt_may_throw([this]() {
        if (!about_open_)
        {
            new AboutWindow(about_open_, this);
        }
    });
}

void MainWindow::enable_emulation_actions(bool should)
{
    stop_act_->setEnabled(should);
    reset_act_->setEnabled(should);
    if (should)
        screen_->show();
    else
        screen_->hide();
}

void MainWindow::initialize_emulator_data()
{
    for (int i = 0; i < EmuTypeSize; i++)
    {
        std::string file_name = hydra::serialize_emu_type(static_cast<hydra::EmuType>(i));
        std::transform(file_name.begin(), file_name.end(), file_name.begin(), ::tolower);
        auto path = ":/emulators/" + file_name + ".json";

        QFile file(QString::fromStdString(path));
        if (!file.open(QIODevice::ReadOnly))
        {
            Logger::Fatal("Failed to open emulator data file: {}", path);
        }

        std::string file_data = file.readAll().toStdString();
        using json = nlohmann::json;
        json j = json::parse(file_data);
        hydra::UiCommon::EmulatorData[i].Name = j["Name"].get<std::string>();
        hydra::UiCommon::EmulatorData[i].Extensions =
            j["Extensions"].get<std::vector<std::string>>();
    }
}

void MainWindow::pause_emulator()
{
    paused_ = !paused_;
    if (paused_)
    {
        emulator_timer_->stop();
    }
    else
    {
        emulator_timer_->start();
    }
}

void MainWindow::reset_emulator()
{
    if (emulator_)
    {
        std::unique_lock<std::mutex> alock(audio_mutex_);
        queued_audio_.clear();
        emulator_->Reset();
    }
}

void MainWindow::stop_emulator()
{
    if (emulator_)
    {
        std::unique_lock<std::mutex> alock(audio_mutex_);
        emulator_timer_->stop();
        queued_audio_.clear();
        emulator_.reset();
        enable_emulation_actions(false);
        video_info_ = {};
    }
}

void MainWindow::emulator_frame()
{
    std::unique_lock<std::mutex> elock(emulator_mutex_);
    std::future<void> frame = emulator_->RunFrameAsync();
    frame.wait();

    screen_->Redraw(video_info_.width, video_info_.height, video_info_.data.data());
}

void MainWindow::video_callback(const hydra::VideoInfo& vi)
{
    video_info_ = vi;
}

void MainWindow::audio_callback(const hydra::AudioInfo& ai)
{
    std::unique_lock<std::mutex> lock(audio_mutex_);
    queued_audio_.reserve(queued_audio_.size() + ai.data.size());
    queued_audio_.insert(queued_audio_.end(), ai.data.begin(), ai.data.end());
}

void MainWindow::poll_input_callback() {}

int8_t MainWindow::read_input_callback(const hydra::InputInfo& ii)
{
    return input_state_[ii.button];
}

void MainWindow::add_recent(const std::string& path)
{
    auto it = std::find(recent_files_.begin(), recent_files_.end(), path);
    if (it != recent_files_.end())
    {
        recent_files_.erase(it);
    }
    recent_files_.push_front(path);
    if (recent_files_.size() > 10)
    {
        recent_files_.pop_back();
    }

    update_recent_files();
}

void MainWindow::update_recent_files()
{
    QMenu* menu = new QMenu;
    for (size_t i = 0; i < recent_files_.size(); i++)
    {
        Settings::Set("recent_" + std::to_string(i), recent_files_[i]);
        std::string path = recent_files_[i];
        menu->addAction(path.c_str(), [this, path]() { open_file_impl(path); });
    }
    if (recent_act_->menu())
        recent_act_->menu()->deleteLater();
    recent_act_->setMenu(menu);
}