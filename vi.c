#include "vi.h"
#include "ntop.h"
#include "util.h"
#include <conio.h>
#include <stdio.h>
#include <Windows.h>

TCHAR *CurrentInputStr;
TCHAR *ViErrorMessage;
int InInputMode = 0;
static DWORD InputIndex;

#define DEFAULT_STR_SIZE 1024

typedef int (*cmd_fn)(DWORD Argc, TCHAR **Argv);

typedef struct cmd {
	TCHAR *Name;
	cmd_fn CmdFunc;
} cmd;

#define COMMAND_FUNC(Name) static int Name##_func(DWORD Argc, TCHAR **Argv)
#define COMMAND_ALIAS(Name, AliasName) static int Name##_func(DWORD Argc, TCHAR **Argv) { return AliasName##_func(Argc, Argv); }
#define COMMAND(Name) { _T(#Name), Name##_func }

static TCHAR *History[256];
static DWORD HistoryCount;
static DWORD HistoryBufSize;
static DWORD HistoryIndex;

static void PushToHistory(TCHAR *Str)
{
	int Length = _tcsclen(Str);
	History[HistoryCount] = xmalloc(sizeof **History * (Length + 1));
	_tcscpy_s(History[HistoryCount], Length+1, Str);
	HistoryCount++;
	HistoryIndex++;
}

static void HistoryPrevious(TCHAR **Str)
{
	HistoryIndex--;
	_tcscpy_s(*Str, DEFAULT_STR_SIZE, History[HistoryIndex]);
	InputIndex = _tcsclen(*Str);
}

static void HistoryNext(TCHAR **Str)
{
	HistoryIndex++;
	_tcscpy_s(*Str, DEFAULT_STR_SIZE, History[HistoryIndex]);
	InputIndex = _tcsclen(*Str);
}

static void SetViError(TCHAR *Fmt, ...)
{
	va_list VaList;

	va_start(VaList, Fmt);
	_vstprintf_s(ViErrorMessage, DEFAULT_STR_SIZE, Fmt, VaList);
	va_end(VaList);
}

COMMAND_FUNC(kill)
{
	if(Argc == 0) {
		SetViError(_T("Usage: kill PID(s)"));
		return 1;
	}

	for(DWORD i = 0; i < Argc; i++) {
		DWORD Pid = _tcstoul(Argv[i], NULL, 10);

		/*
		 * strtoul returns 0 when conversion failed and we cannot kill pid 0 anyway
		 */
		if(Pid == 0) {
			SetViError(_T("Not a valid pid: %s"), Argv[i]);
			continue;
		}

		HANDLE Handle = OpenProcess(PROCESS_TERMINATE, FALSE, Pid);
		if(!Handle) {
			SetViError(_T("Could not open process: %ld: 0x%08x"), Pid, GetLastError());
			continue;
		}

		if(!TerminateProcess(Handle, 9)) {
			SetViError(_T("Failed to kill process: %ld: 0x%08x"), Pid, GetLastError());
			CloseHandle(Handle);
			return 1;
		}

		CloseHandle(Handle);
	}

	return 0;
}


COMMAND_FUNC(tree)
{
	if(Argc != 0) {
		SetViError(_T("Error: trailing characters"));
		return 1;
	}

	ChangeProcessSortType(SORT_BY_TREE);

	return 0;
}

COMMAND_FUNC(exec)
{
	if(Argc != 1) {
		SetViError(_T("Usage: exec COMMAND"));
		return 1;
	}

	STARTUPINFO StartupInfo;
	PROCESS_INFORMATION ProcInfo;
	ZeroMemory(&StartupInfo, sizeof(StartupInfo));
	ZeroMemory(&ProcInfo, sizeof(ProcInfo));
	StartupInfo.cb = sizeof(StartupInfo);

	BOOL Ret = CreateProcess(NULL, Argv[0], NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcInfo);

	if(!Ret) {
		SetViError(_T("Failed to create process: 0x%08x"), GetLastError());
		return 1;
	}

	return 1;
}

COMMAND_FUNC(q)
{
	exit(EXIT_SUCCESS);
	return 0;
}
COMMAND_ALIAS(quit, q);

COMMAND_FUNC(sort)
{
	if(Argc != 1) {
		SetViError(_T("Usage: sort COLUMN"));
		return 1;
	}

	process_sort_type NewSortType;
	if(!GetProcessSortTypeFromName(Argv[0], &NewSortType)) {
		SetViError(_T("Unknown column: %s"), Argv[0]);
	}

	ChangeProcessSortType(NewSortType);

	return 0;
}

static cmd Commands[] = {
	COMMAND(exec),
	COMMAND(kill),
	COMMAND(q),
	COMMAND(quit),
	COMMAND(sort),
	COMMAND(tree),
};

typedef struct cmd_parse_result {
	TCHAR *Name;
	DWORD Argc;
	TCHAR **Args;
} cmd_parse_result;

static void PushArg(cmd_parse_result *ParseResult)
{
	++ParseResult->Argc;

	if(ParseResult->Args == NULL) {
		ParseResult->Args = xmalloc(1 * sizeof *ParseResult->Args);
	} else {
		ParseResult->Args = xrealloc(ParseResult->Args, ParseResult->Argc * sizeof *ParseResult->Args);
	}

	ParseResult->Args[ParseResult->Argc-1] = xcalloc(DEFAULT_STR_SIZE, sizeof(TCHAR));
}

static void FreeCmdParseResult(cmd_parse_result *ParseResult)
{
	free(ParseResult->Name);
	for(DWORD i = 0; i < ParseResult->Argc; i++) {
		free(ParseResult->Args[i]);
	}
	free(ParseResult->Args);
}

static TCHAR *EatSpaces(TCHAR *Str)
{
	while(_istspace(*Str)) {
		Str++;
	}
	return Str;
}

static BOOL IsValidCharacter(TCHAR c)
{
	return (_istalnum(c) || c == _T('%'));
}

static BOOL ParseCommand(TCHAR *Str, cmd_parse_result *Result)
{
	Result->Argc = 0;
	Result->Args = NULL;

	Result->Name = xmalloc(DEFAULT_STR_SIZE * sizeof *Result->Name);

	Str = EatSpaces(Str);

	int i = 0;

	/*
	 * TODO: should allow for commands to contain digits but not start with them
	 */
	while(_istalpha(*Str)) {
		Result->Name[i++] = *Str++;
	}

	Result->Name[i] = '\0';

	/*
	 * Do error when we stopped on an non-alphanumeric character and not on a
	 * terminator or space
	 */
	if(*Str != _T('\0') && !_istspace(*Str)) {
		/* parse error */
		free(Result->Name);
		return FALSE;
	}

	int ArgIndex = 0;
	DWORD ArgsSize = 16;

	int InQuotes = FALSE;

	if(*Str != '\0') {
		i = 0;

		/*
		 * Read arguments
		 */
		while(*Str != '\0') {
			Str = EatSpaces(Str);

			if(*Str == '\0') {
				break;
			}

			if(*Str == _T('\"')) {
				InQuotes = TRUE;
				Str++;
			}

			PushArg(Result);

			int j = 0;
			if(InQuotes) {
				/*
				 * When inside quotes read tokens and spaces inbetween
				 * until we hit a closing quote or terminator
				 */
				do {
					while(_istspace(*Str) || IsValidCharacter(*Str)) {
						Result->Args[i][j++] = *Str++;
					}

					if(*Str == '\"') {
						++Str;
						InQuotes = FALSE;
						break;
					}
				} while(*Str != '\0');
			} else {
				/*
				 * When not inside quotes then only read single token
				 */
				while(IsValidCharacter(*Str)) {
					Result->Args[i][j++] = *Str++;
				}
			}

			Result->Args[i][j] = '\0';
			++i;

			/*
			 * Do error and clean up if either the string wasn't closed
			 * or if we hit a non-alphanumeric character or non space
			 */
			if(InQuotes || (*Str != _T('\0') && !_istspace(*Str))) {
				/* parse error */
				FreeCmdParseResult(Result);
				return FALSE;
			}
		}
	}

	return TRUE;
}

static void TryExec(TCHAR *Str)
{
	cmd_parse_result ParseResult;

	if(!ParseCommand(Str, &ParseResult)) {
		SetViError(_T("parse error"));
		return;
	}

	for(DWORD i = 0; i < _countof(Commands); i++) {
		cmd *Command = &Commands[i];

		if(_tcsicmp(Command->Name, ParseResult.Name) == 0) {
			/* TODO: what to do with the error? */
			int Ret = Command->CmdFunc(ParseResult.Argc, ParseResult.Args);
			FreeCmdParseResult(&ParseResult);
			return;
		}
	}

	SetViError(_T("Not an editor command: %s"), ParseResult.Name);
	FreeCmdParseResult(&ParseResult);
}

void ViInit(void)
{
	CurrentInputStr = xcalloc(DEFAULT_STR_SIZE, 1);
	ViErrorMessage = xcalloc(DEFAULT_STR_SIZE, 1);
}

void ViEnableInput(void)
{
	memset(CurrentInputStr, 0, DEFAULT_STR_SIZE * sizeof *CurrentInputStr);
	_tcscpy(CurrentInputStr, _T(":"));

	ViErrorMessage[0] = 0;
	InInputMode = 1;
	InputIndex = 1;
}

void ViDisableInput(void)
{
	CurrentInputStr[0] = 0;
	InInputMode = 0;

	FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
}

int ViHandleInputKey(KEY_EVENT_RECORD *KeyEvent)
{
	switch(KeyEvent->wVirtualKeyCode) {
	case VK_UP:
		if(HistoryCount != 0 && HistoryIndex != 0) {
			HistoryPrevious(&CurrentInputStr);
			return 1;
		}
		break;
	case VK_DOWN:
		if(HistoryIndex < HistoryCount-1) {
			HistoryNext(&CurrentInputStr);
			return 1;
		} 
		break;
	case VK_BACK:
		if(InputIndex != 0) {
			CurrentInputStr[--InputIndex] = '\0';
			if(InputIndex == 0) {
				ViDisableInput();
			}
			return 1;
		}
		break;
	case VK_ESCAPE:
		ViDisableInput();
		return 1;
	case VK_RETURN:
		ViExecInput();
		return 1;
	default:
		if(isprint(KeyEvent->uChar.AsciiChar)) {
			CurrentInputStr[InputIndex++] = KeyEvent->uChar.AsciiChar;
			return 1;
		}
	}

	return 0;
}

void ViExecInput(void)
{
	TCHAR *Cmd = CurrentInputStr;

	/*
	 * Ignore all preceeding colons and spaces
	 */
	while(*Cmd == ':' || _istspace(*Cmd)) {
		Cmd++;
	}

	/*
	 * We check for empty string here because empty strings shouldn't throw errors
	 */
	if(*Cmd != '\0') {
		TryExec(Cmd);
		PushToHistory(CurrentInputStr);
	}

	ViDisableInput();
}
