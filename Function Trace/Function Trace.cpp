// Function Trace.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <Windows.h>
#include <string>
#include <unordered_map>
#include <iostream>

#include <inttypes.h>
#include "capstone/capstone.h"


std::unordered_map<DWORD, BYTE> softBreakPoint;   //CodeAddress -> originalByte
std::unordered_map<DWORD, HANDLE> ThreadHandles;  //ThreadId -> ThreadHandle
std::unordered_map<DWORD, int> ThreadCallDepth;  //ThreadId -> ThreadHandle

//D:\KCleaner.exe
bool SetBreakPoint(HANDLE hProcess ,DWORD addr) {
	DWORD dwStartAddress = addr;
	BYTE cInstruction,m_OriginalInstruction;
	DWORD dwReadBytes;
	bool success;

	// Read the first instruction
	ReadProcessMemory(hProcess, (void*)dwStartAddress, &cInstruction, 1, &dwReadBytes);

	// Save it!
	m_OriginalInstruction = cInstruction;

	// Replace it with Breakpoint
	cInstruction = 0xCC;
	WriteProcessMemory(hProcess, (void*)dwStartAddress, &cInstruction, 1, &dwReadBytes);
	success = FlushInstructionCache(hProcess, (void*)dwStartAddress, 1);
	softBreakPoint[addr] = m_OriginalInstruction;
	return success;
}

bool ReadSomeCode(HANDLE hProcess, DWORD addr, BYTE *arr,size_t size) {
	DWORD dwReadBytes;
	return ReadProcessMemory(hProcess, (void*)addr, arr, size, &dwReadBytes);
}

bool RemoveBreakPoint(HANDLE hProcess, DWORD addr) {
	DWORD dwReadBytes;
	bool success=WriteProcessMemory(hProcess, (void*)addr, &softBreakPoint[addr], 1, &dwReadBytes);
	FlushInstructionCache(hProcess, (void*)addr, 1);
	return success;
}

void setTrapFlag(HANDLE hThread) {
	CONTEXT lcContext;
	lcContext.ContextFlags = CONTEXT_ALL;
	GetThreadContext(hThread, &lcContext);
	lcContext.EFlags |= 0x100; //Active Trap Flag
	SetThreadContext(hThread, &lcContext);
}

void BackEIP(HANDLE hThread) {
	CONTEXT lcContext;
	lcContext.ContextFlags = CONTEXT_ALL;
	GetThreadContext(hThread, &lcContext);
	lcContext.Eip--;
	SetThreadContext(hThread, &lcContext);
}

int main()
{
	std::wstring imageName;
    std::cout << "실행할 프로그램 : ";
	//std::wcin >> imageName;
	imageName = std::wstring(TEXT("D:\\Kcleaner.exe"));
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	bool a;
	a=CreateProcess(imageName.c_str(), NULL, NULL, NULL, FALSE,
		DEBUG_PROCESS, NULL, NULL, &si, &pi);

	DebugBreakProcess(pi.hProcess);

	bool debuggingState = true;
	int cnt = 0;
	DEBUG_EVENT debug_event = { 0 };
	csh cHandle;
	cs_insn *insn;
	size_t insCnt;
	BYTE Codes[17];
	if (cs_open(CS_ARCH_X86, CS_MODE_32, &cHandle) != CS_ERR_OK) {
		return -1;
	}
	while (debuggingState)
	{
		if (!WaitForDebugEvent(&debug_event, INFINITE))
			return 0;
		
		bool success = false;
		switch (debug_event.dwDebugEventCode)
		{
		case CREATE_PROCESS_DEBUG_EVENT:
			printf("새로운 프로세스 생성\n");
		case CREATE_THREAD_DEBUG_EVENT:

			printf("Thread 0x%x (Id: %d) created at: 0x%x\n",
				(unsigned int)(debug_event.u.CreateThread.hThread),
				debug_event.dwThreadId,
				(unsigned int)debug_event.u.CreateThread.lpStartAddress);
			setTrapFlag(debug_event.u.CreateThread.hThread);
			ThreadHandles[debug_event.dwThreadId] = debug_event.u.CreateThread.hThread;     //ThreadId에 대한 핸들 저장
			ThreadCallDepth[debug_event.dwThreadId]=0;
			break;

		case EXIT_THREAD_DEBUG_EVENT:
			printf("The thread %d exited with code: %d\n",
				debug_event.dwThreadId,
				debug_event.u.ExitThread.dwExitCode);    // The thread 2760 exited with code: 0

			if (ThreadHandles.count(debug_event.dwThreadId)) {  //저장된 핸들 있으면 핸들 삭제
				ThreadHandles.erase(debug_event.dwThreadId);
			}
			if (ThreadCallDepth.count(debug_event.dwThreadId)) {  //저장된 Call Depth 삭제
				ThreadCallDepth.erase(debug_event.dwThreadId);
			}

			break;
		case EXIT_PROCESS_DEBUG_EVENT:

			printf("Process exited with code:  0x%x",
				debug_event.u.ExitProcess.dwExitCode);
			debuggingState = false;
			break;
		case EXCEPTION_DEBUG_EVENT:

			EXCEPTION_DEBUG_INFO& exception = debug_event.u.Exception;
			DWORD ExceptionPos = (DWORD)debug_event.u.Exception.ExceptionRecord.ExceptionAddress;
			
			switch (exception.ExceptionRecord.ExceptionCode)
			{
			case EXCEPTION_BREAKPOINT:
				if (softBreakPoint.count(ExceptionPos)) { //브레이크포인트 걸려있으면?
					success = RemoveBreakPoint(pi.hProcess,ExceptionPos);
					BackEIP(ThreadHandles[debug_event.dwThreadId]);
					setTrapFlag(ThreadHandles[debug_event.dwThreadId]);
				}
				else {
					printf("없는데 브포 발생\n");
				}

				ReadSomeCode(pi.hProcess, ExceptionPos, Codes, 16);  //maximun instruction length
				insCnt = cs_disasm(cHandle, Codes, sizeof(Codes) - 1, ExceptionPos, 0, &insn);

				if (insCnt) {
					printf("%d - 0x%" PRIx64 " : %s\t%s\n", debug_event.dwThreadId, insn[0].address, insn[0].mnemonic, insn[0].op_str);
				}

				break;
			case EXCEPTION_SINGLE_STEP:
				ReadSomeCode(pi.hProcess, ExceptionPos, Codes, 16);
				insCnt = cs_disasm(cHandle, Codes, sizeof(Codes) - 1, ExceptionPos, 0, &insn);

				if (insCnt) {
					if (std::string(insn[0].mnemonic).find("call",0)==0) {
						printf("%d - 0x%" PRIx64 " : ", debug_event.dwThreadId, insn[0].address);
						for (int i = 0; i < ThreadCallDepth[debug_event.dwThreadId]; i++) {
							printf("  ");
						}
						printf("%s %s\n", insn[0].mnemonic, insn[0].op_str);
						ThreadCallDepth[debug_event.dwThreadId]++;
					}
					else if (std::string(insn[0].mnemonic).find("ret", 0) == 0) {
						printf("%d - 0x%" PRIx64 " : ", debug_event.dwThreadId, insn[0].address);
						for (int i = 0; i < ThreadCallDepth[debug_event.dwThreadId]; i++) {
							printf("  ");
						}
						printf("%s %s\n", insn[0].mnemonic, insn[0].op_str);
						ThreadCallDepth[debug_event.dwThreadId]--;
					}
				}
				if (ThreadHandles.count(debug_event.dwThreadId)) {  //핸들이 존재한다는거는 SoftwareBP를 걸었다는 뜻
					setTrapFlag(ThreadHandles[debug_event.dwThreadId]);  //TrapFlag는 리셋되므로 다시 설정
				}
				break;
			case EXCEPTION_ACCESS_VIOLATION:
				printf("Access Violation!!!\n");
				scanf_s("");
				break;
			default:
				printf("%x", exception.ExceptionRecord.ExceptionCode);
				break;
			}
			break;
		}
		
		ContinueDebugEvent(debug_event.dwProcessId,
			debug_event.dwThreadId,
			DBG_CONTINUE);
	}

	return 0;
}
