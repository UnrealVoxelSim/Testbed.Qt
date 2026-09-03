#include "Window.h"

#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Profiling/Api/NullRecorder.h"
#include "UnrealVoxelSim/Testbed/World.h"
#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
#include "UnrealVoxelSim/Profiling/Tracy/Recorder.h"
#endif

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <string>

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);

#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
    UnrealVoxelSim::Profiling::Tracy::Recorder profiling;
#else
    UnrealVoxelSim::Profiling::Api::NullRecorder profiling;
#endif
    UNREALVOXELSIM_PROFILE_THREAD(profiling, "Qt main");

    QCommandLineParser commandLine;
    commandLine.setApplicationDescription("UnrealVoxelSim interactive Qt testbed");
    commandLine.addHelpOption();
    const QCommandLineOption worldOption{{"w", "world"}, "Select the initial test world.", "name", "standard"};
    const QCommandLineOption smokeTestOption{"smoke-test", "Exit automatically after startup."};
    commandLine.addOption(worldOption);
    commandLine.addOption(smokeTestOption);
    commandLine.process(application);

    const auto worldName = commandLine.value(worldOption).toStdString();
    const auto worlds = UnrealVoxelSim::Testbed::WorldCatalog::Worlds();
    if (std::ranges::find(worlds, worldName, [](const UnrealVoxelSim::Testbed::WorldDescriptor &world) {
            return world.Id;
        }) == worlds.end())
    {
        QMessageBox::critical(nullptr, "Testbed initialization", "Unknown world selected with --world.");
        return 2;
    }

    try
    {
        UnrealVoxelSim::Testbed::Qt::Window window{worldName, profiling};
        window.resize(1280, 800);
        window.show();

        if (commandLine.isSet(smokeTestOption))
        {
            QTimer::singleShot(1000, &application, [&application, &window] {
                if (!window.TextureResourcesReady()) application.exit(4);
            });
            if (worldName == "stress")
            {
                QTimer::singleShot(250, &application,
                                   [&window] { window.CurrentWorld().SetTargetPopulation(900); });
                QTimer::singleShot(500, &application,
                                   [&window] { window.CurrentWorld().SetTargetPopulation(1'000); });
                QTimer::singleShot(750, &application, [&application, &window] {
                    if (window.CurrentWorld().Pawns().size() != 1'000) application.exit(3);
                });
            }
            QTimer::singleShot(1200, &application, &QApplication::quit);
        }
        return application.exec();
    }
    catch (const std::exception &)
    {
        return 1;
    }
}
