#pragma once

#include <windows.h>

#include "module_scanner.h"
#include "../utils/threads_util.h"
#include "../utils/process_symbols.h"
#include "../stats/stats.h"
#include "../stats/entropy_stats.h"
#include <vector>

namespace pesieve {

	typedef enum ThSusIndicator {
		THI_NONE,
		THI_SUS_START,
		THI_SUS_IP,
		THI_SUS_RET,
		THI_SUS_CALLSTACK_SHC,
		THI_SUS_CALLS_INTEGRITY,
		THI_SUS_CALLSTACK_CORRUPT,
		THI_MAX
	} _ThSusIndicator;

	inline std::string indicator_to_str(const ThSusIndicator& indicator)
	{
		switch (indicator) {
			case THI_NONE: return "NONE";
			case THI_SUS_START: return "SUS_START";
			case THI_SUS_IP: return "SUS_IP";
			case THI_SUS_RET: return "SUS_RET";
			case THI_SUS_CALLSTACK_SHC: return "SUS_CALLSTACK_SHC";
			case THI_SUS_CALLS_INTEGRITY: return "SUS_CALLS_INTEGRITY";
			case THI_SUS_CALLSTACK_CORRUPT: return "SUS_CALLSTACK_CORRUPT";
		}
		return "";
	}

	//!  A custom structure keeping a fragment of a thread context
	typedef struct _ctx_details {
		bool is64b;
		ULONGLONG rip;
		ULONGLONG rsp;
		ULONGLONG rbp;
		ULONGLONG last_ret; // the last return address on the stack
		ULONGLONG ret_on_stack; // the last return address stored on the stack
		bool is_ret_as_syscall;
		bool is_ret_in_frame;
		bool is_managed; // does it contain .NET modules
		std::vector<ULONGLONG> callStack;

		_ctx_details(bool _is64b = false, ULONGLONG _rip = 0, ULONGLONG _rsp = 0, ULONGLONG _rbp = 0, ULONGLONG _ret_addr = 0)
			: is64b(_is64b), rip(_rip), rsp(_rsp), rbp(_rbp), last_ret(_ret_addr), ret_on_stack(0), is_ret_as_syscall(true), is_ret_in_frame(true),
			is_managed(false)
		{
		}

		void init(bool _is64b = false, ULONGLONG _rip = 0, ULONGLONG _rsp = 0, ULONGLONG _rbp = 0, ULONGLONG _ret_addr = 0)
		{
			this->is64b = _is64b;
			this->rip = _rip;
			this->rsp = _rsp;
			this->rbp = _rbp;
			this->last_ret = _ret_addr;
		}

	} ctx_details;

	//!  A report from the thread scan, generated by ThreadScanner
	class ThreadScanReport : public ModuleScanReport
	{
	public:
		static const DWORD THREAD_STATE_UNKNOWN = (-1);
		static const DWORD THREAD_STATE_WAITING = 5;

		static std::string translate_thread_state(DWORD thread_state);
		static std::string translate_wait_reason(DWORD thread_wait_reason);
		
		//---

		ThreadScanReport(DWORD _tid)
			: ModuleScanReport(0, 0), 
			tid(_tid), 
			susp_addr(0), protection(0), stack_ptr(0),
			thread_state(THREAD_STATE_UNKNOWN), 
			thread_wait_reason(0), thread_wait_time(0), is_code(false)
		{
		}

		const virtual void callstackToJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level& jdetails)
		{
			bool printCallstack = (jdetails >= JSON_DETAILS) ? true : false;
			if (this->indicators.find(THI_SUS_CALLSTACK_CORRUPT) != this->indicators.end()) {
				printCallstack = true;
			}
			if (this->indicators.find(THI_SUS_CALLSTACK_SHC) != this->indicators.end()) {
				printCallstack = true;
			}
			OUT_PADDED(outs, level, "\"stack_ptr\" : ");
			outs << "\"" << std::hex << stack_ptr << "\"";
			if (cDetails.callStack.size()) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"frames_count\" : ");
				outs << std::dec << cDetails.callStack.size();
				if (printCallstack) {
					outs << ",\n";
					OUT_PADDED(outs, level, "\"frames\" : [");
					for (auto itr = cDetails.callStack.rbegin(); itr != cDetails.callStack.rend(); ++itr) {
						if (itr != cDetails.callStack.rbegin()) {
							outs << ", ";
						}
						const ULONGLONG addr = *itr;
						outs << "\"" << std::hex << addr;
						auto sItr = this->addrToSymbol.find(addr);
						if (sItr != this->addrToSymbol.end()) {
							outs << ";" << sItr->second;
						}
						outs << "\"";
						
					}
					outs << "]";
				}
			}
		}

		const bool moduleInfoToJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level& jdetails)
		{
			if (!this->module) {
				return false;
			}
			outs << ",\n";
			OUT_PADDED(outs, level, "\"module\" : ");
			outs << "\"" << std::hex << (ULONGLONG)module << "\"";
			if (moduleSize) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"module_size\" : ");
				outs << "\"" << std::hex << (ULONGLONG)moduleSize << "\"";
			}
			outs << ",\n";
			OUT_PADDED(outs, level, "\"protection\" : ");
			outs << "\"" << std::hex << protection << "\"";
			if (stats.isFilled()) {
				outs << ",\n";
				stats.toJSON(outs, level);
			}
			return true;
		}

		const bool threadInfoToJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level& jdetails)
		{
			OUT_PADDED(outs, level, "\"state\" : ");
			if (thread_state == THREAD_STATE_UNKNOWN) {
				outs << "\"" << "UNKNOWN" << "\"";
			}
			else {
				outs << "\"" << translate_thread_state(thread_state) << "\"";
			}
			if (thread_state == THREAD_STATE_WAITING) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"wait_reason\" : ");
				outs << "\"" << translate_wait_reason(thread_wait_reason) << "\"";
			}
			if (stack_ptr) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"callstack\" : {\n");
				callstackToJSON(outs, level + 1, jdetails);
				outs << "\n";
				OUT_PADDED(outs, level, "}");
			}
			bool showLastCall = (jdetails >= JSON_DETAILS) ? true : false;
			if ((this->indicators.find(THI_SUS_CALLS_INTEGRITY) != this->indicators.end()) || 
				(this->indicators.find(THI_SUS_CALLSTACK_CORRUPT) != this->indicators.end()) )
			{
				showLastCall = true;
			}
			if (showLastCall) {
				if (!this->lastSyscall.empty()) {
					outs << ",\n";
					OUT_PADDED(outs, level, "\"last_sysc\" : ");
					outs << "\"" << this->lastSyscall << "\"";
				}
				if (!this->lastFunction.empty() && (this->lastFunction != this->lastSyscall)) {
					outs << ",\n";
					OUT_PADDED(outs, level, "\"last_func\" : ");
					outs << "\"" << this->lastFunction << "\"";
				}
			}
			outs << "\n";
			return true;
		}

		const bool indicatorsToJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level& jdetails)
		{
			OUT_PADDED(outs, level, "\"indicators\" : [");
			for (auto itr = indicators.begin(); itr != indicators.end(); ++itr) {
				if (itr != indicators.begin()) {
					outs << ", ";
				}
				outs << "\"" << indicator_to_str(*itr) << "\"";
			}
			outs << "]";
			return true;
		}

		const virtual void fieldsToJSON(std::stringstream &outs, size_t level, const pesieve::t_json_level &jdetails)
		{
			ElementScanReport::_toJSON(outs, level);
			outs << ",\n";
			OUT_PADDED(outs, level, "\"thread_id\" : ");
			outs << std::dec << tid;
			outs << ",\n";
			OUT_PADDED(outs, level, "\"thread_info\" : {\n");
			threadInfoToJSON(outs, level + 1, jdetails);
			OUT_PADDED(outs, level, "}");
			outs << ",\n";
			indicatorsToJSON(outs, level, jdetails);

			if (susp_addr) {
				outs << ",\n";
				if (this->module && this->moduleSize) {
					OUT_PADDED(outs, level, "\"susp_addr\" : ");
				}
				else {
					OUT_PADDED(outs, level, "\"susp_return_addr\" : ");
				}
				outs << "\"" << std::hex << susp_addr << "\"";
			}
			moduleInfoToJSON(outs, level, jdetails);
		}

		const virtual bool toJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level &jdetails)
		{
			OUT_PADDED(outs, level, "\"thread_scan\" : {\n");
			fieldsToJSON(outs, level + 1, jdetails);
			outs << "\n";
			OUT_PADDED(outs, level, "}");
			return true;
		}

		DWORD tid;
		ULONGLONG susp_addr;
		DWORD protection;
		ULONGLONG stack_ptr;
		DWORD thread_state;
		DWORD thread_wait_reason;
		DWORD thread_wait_time;

		std::string lastSyscall;
		std::string lastFunction;

		ctx_details cDetails;
		std::map<ULONGLONG, std::string> addrToSymbol;
		std::set<ULONGLONG> shcCandidates;
		std::set<ThSusIndicator> indicators;

		AreaEntropyStats stats;
		bool is_code;
	};

	//!  A scanner for threads
	//!  Stack-scan inspired by the idea presented here: https://github.com/thefLink/Hunt-Sleeping-Beacons
	class ThreadScanner : public ProcessFeatureScanner {
	public:
		ThreadScanner(HANDLE hProc, bool _isReflection, bool _isManaged, const util::thread_info& _info, ModulesInfo& _modulesInfo, peconv::ExportsMapper* _exportsMap, ProcessSymbolsManager* _symbols)
			: ProcessFeatureScanner(hProc), isReflection(_isReflection), isManaged(_isManaged),
			info(_info), modulesInfo(_modulesInfo), exportsMap(_exportsMap), symbols(_symbols)
		{
		}

		virtual ThreadScanReport* scanRemote();

	protected:
		void initReport(ThreadScanReport& my_report);
		void reportResolvedCallstack(ThreadScanReport& my_report);
		static std::string choosePreferredFunctionName(const std::string& dbgSymbol, const std::string& manualSymbol);

		bool scanRemoteThreadCtx(HANDLE hThread, ThreadScanReport& my_report);
		bool fetchThreadCtxDetails(IN HANDLE hProcess, IN HANDLE hThread, OUT ThreadScanReport& my_report);

		bool isAddrInNamedModule(ULONGLONG addr);
		void printThreadInfo(const util::thread_info& threadi);
		std::string resolveLowLevelFuncName(IN const ULONGLONG addr, OUT OPTIONAL size_t* disp = nullptr);
		std::string resolveAddrToString(IN ULONGLONG addr);
		bool printResolvedAddr(const ULONGLONG addr);
		size_t fillCallStackInfo(IN HANDLE hProcess, IN HANDLE hThread, IN LPVOID ctx, IN OUT ThreadScanReport& my_report);
		size_t analyzeCallStackInfo(IN OUT ThreadScanReport& my_report);
		size_t _analyzeCallStack(IN OUT ctx_details& cDetails, OUT IN std::set<ULONGLONG>& shcCandidates);

		bool checkReturnAddrIntegrity(IN const std::vector<ULONGLONG>& callStack, IN OUT ThreadScanReport& my_report);

		bool fillAreaStats(ThreadScanReport* my_report);
		bool reportSuspiciousAddr(ThreadScanReport* my_report, ULONGLONG susp_addr);
		bool filterDotNet(ThreadScanReport& my_report);

		bool isReflection;
		bool isManaged;
		const util::thread_info& info;
		ModulesInfo& modulesInfo;
		peconv::ExportsMapper* exportsMap;
		ProcessSymbolsManager* symbols;
	};

}; //namespace pesieve
