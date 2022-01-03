#pragma once
#ifndef TKP_EMULATOR_H
#define TKP_EMULATOR_H
#include <SDL2/SDL.h>
#include <iosfwd>
#include <mutex>
#include <vector>
#include <atomic>
#include <functional>
#include <thread>
#include "TKPImage.h"
#include "emulator_results.h"
#include "disassembly_instr.h"
namespace TKPEmu {
	enum class EmuStartOptions {
		Normal,
		Debug,
		Console
	};
	class Emulator {
	private:
		using TKPImage = TKPEmu::Tools::TKPImage;
		using DisInstr = TKPEmu::Tools::DisInstr;
	public:
		Emulator() = default;
		virtual ~Emulator() = default;
		Emulator(const Emulator&) = delete;
		Emulator& operator=(const Emulator&) = delete;
		std::atomic_bool Stopped = false;
		std::atomic_bool Paused = false;
		std::atomic_bool Step = false;
		std::atomic_bool Loaded = false;
		std::atomic_int InstructionBreak = -1;
		bool SkipBoot = false;
		bool FastMode = false;
		unsigned long long TotalClocks = 0;
		unsigned long long ScreenshotClocks = 0;
		void Start(EmuStartOptions start_mode);
		void Reset();
		virtual void HandleKeyDown(SDL_Keycode keycode);
		virtual void HandleKeyUp(SDL_Keycode keycode);
		virtual void SaveState(std::string filename);
		virtual void LoadState(std::string filename);
		void LoadFromFile(std::string path);
		void StartLogging(std::string filename);
		void StopLogging();
		void Screenshot(std::string filename);
		void CloseAndWait();
		virtual float* GetScreenData();
		virtual std::string GetScreenshotHash();
		virtual std::string GetEmulatorName();
		virtual bool IsReadyToDraw() { return false; };
		std::mutex ThreadStartedMutex;
		std::mutex DrawMutex;
		std::mutex DebugUpdateMutex;
		std::thread UpdateThread;
		TKPImage EmulatorImage{};
		std::string RomHash;
		std::string ScreenshotHash;
		std::string CurrentFilename;
		TKPEmu::Testing::TestResult Result = TKPEmu::Testing::TestResult::Unknown; 
	protected:
		// To be placed at the end of your update function
		// Override v_log_state() to change what it does, log_state() will do the right
		// checks for you
		void log_state();
		std::unique_ptr<std::ofstream> ofstream_ptr_;
		EmuStartOptions start_options = EmuStartOptions::Console;
	private:
		virtual void save_state(std::ofstream& ofstream);
		virtual void load_state(std::ifstream& ifstream);
		virtual void v_log_state();
		virtual void start_normal();
		virtual void start_debug();
		virtual void start_console();
		virtual void reset_normal();
		virtual void reset_skip();
		virtual void update();
		virtual void load_file(std::string);
		virtual std::string print() const;
		friend std::ostream& operator<<(std::ostream& os, const Emulator& obj);
		// TODO: move all this to gameboy class only - clean up emulator
		bool logging_ = false;
		bool log_changed_ = false;
		std::string log_filename_;
	};
}
#endif
