// tasks.h — Task Scheduler helpers for Velox. Developer: Sallador.
// Velox needs administrator rights (per-process traffic comes from an ETW kernel-network session).
// To avoid a UAC prompt on every start, the installer registers two tasks that run with the
// highest privileges for the current user:
//   \Sallador\Velox            — started on demand by the shortcuts (no trigger)
//   \Sallador\Velox Autostart  — started at logon (enabled / disabled by "Start with Windows")
#pragma once
#include <windows.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>
#include <stdio.h>
#include <wchar.h>
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "secur32.lib")

#define TASK_FOLDER L"\\Sallador"
#define TASK_RUN    L"Velox"
#define TASK_AUTO   L"Velox Autostart"

static ITaskFolder* TsFolder(BOOL create) {
    ITaskService* s = NULL; ITaskFolder* f = NULL;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&s))) return NULL;
    VARIANT v; VariantInit(&v);
    if (SUCCEEDED(s->Connect(v, v, v, v))) {
        BSTR p = SysAllocString(TASK_FOLDER);
        if (FAILED(s->GetFolder(p, &f)) && create) {
            ITaskFolder* root = NULL; BSTR r = SysAllocString(L"\\");
            if (SUCCEEDED(s->GetFolder(r, &root))) { root->CreateFolder(p, v, &f); root->Release(); }
            SysFreeString(r);
        }
        SysFreeString(p);
    }
    s->Release();
    return f;
}
static IRegisteredTask* TsGet(const WCHAR* name) {
    ITaskFolder* f = TsFolder(FALSE); if (!f) return NULL;
    IRegisteredTask* t = NULL; BSTR n = SysAllocString(name);
    if (FAILED(f->GetTask(n, &t))) t = NULL;
    SysFreeString(n); f->Release();
    return t;
}
static void XmlEsc(WCHAR* out, int n, const WCHAR* s) {
    int k = 0;
    for (; *s && k < n - 7; s++) {
        const WCHAR* r = *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : *s == L'"' ? L"&quot;" : NULL;
        if (r) { while (*r) out[k++] = *r++; } else out[k++] = *s;
    }
    out[k] = 0;
}
// Register (create or replace) a task that runs exe with the highest privileges as the current user.
static BOOL TaskRegister(const WCHAR* name, const WCHAR* exe, const WCHAR* args, BOOL atLogon, BOOL enabled) {
    WCHAR user[256]; ULONG un = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &un)) return FALSE;
    WCHAR eu[512], ee[MAX_PATH * 2], ea[256];
    XmlEsc(eu, 512, user); XmlEsc(ee, MAX_PATH * 2, exe); XmlEsc(ea, 256, args ? args : L"");
    WCHAR* xml = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, 8192 * sizeof(WCHAR));
    if (!xml) return FALSE;
    WCHAR trig[600] = L"";
    if (atLogon) swprintf_s(trig, L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId></LogonTrigger></Triggers>", eu);
    swprintf_s(xml, 8192,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Author>Sallador</Author><Description>Velox — network usage widget (Sallador)</Description></RegistrationInfo>"
        L"%s"
        L"<Principals><Principal id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>%s</Enabled><Hidden>false</Hidden>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>%s</Command><Arguments>%s</Arguments></Exec></Actions>"
        L"</Task>",
        trig, eu, enabled ? L"true" : L"false", ee, ea);
    BOOL ok = FALSE;
    ITaskFolder* f = TsFolder(TRUE);
    if (f) {
        BSTR n = SysAllocString(name), x = SysAllocString(xml);
        VARIANT v; VariantInit(&v);
        IRegisteredTask* t = NULL;
        ok = SUCCEEDED(f->RegisterTask(n, x, TASK_CREATE_OR_UPDATE, v, v, TASK_LOGON_INTERACTIVE_TOKEN, v, &t));
        if (t) t->Release();
        SysFreeString(n); SysFreeString(x); f->Release();
    }
    HeapFree(GetProcessHeap(), 0, xml);
    return ok;
}
static void TaskDelete(const WCHAR* name) {
    ITaskFolder* f = TsFolder(FALSE); if (!f) return;
    BSTR n = SysAllocString(name); f->DeleteTask(n, 0); SysFreeString(n);
    f->Release();
}
static void TaskFolderDeleteIfEmpty() {
    ITaskFolder* f = TsFolder(FALSE); if (!f) return;
    IRegisteredTaskCollection* c = NULL; LONG cnt = 1;
    if (SUCCEEDED(f->GetTasks(TASK_ENUM_HIDDEN, &c)) && c) { c->get_Count(&cnt); c->Release(); }
    f->Release();
    if (cnt) return;
    ITaskService* s = NULL;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&s))) return;
    VARIANT v; VariantInit(&v);
    if (SUCCEEDED(s->Connect(v, v, v, v))) {
        ITaskFolder* root = NULL; BSTR r = SysAllocString(L"\\"), p = SysAllocString(L"Sallador");
        if (SUCCEEDED(s->GetFolder(r, &root))) { root->DeleteFolder(p, 0); root->Release(); }
        SysFreeString(r); SysFreeString(p);
    }
    s->Release();
}
// -1 = no such task, 0 = disabled, 1 = enabled
static int TaskEnabled(const WCHAR* name) {
    IRegisteredTask* t = TsGet(name); if (!t) return -1;
    VARIANT_BOOL e = VARIANT_FALSE; t->get_Enabled(&e); t->Release();
    return e != VARIANT_FALSE;
}
static BOOL TaskSetEnabled(const WCHAR* name, BOOL on) {
    IRegisteredTask* t = TsGet(name); if (!t) return FALSE;
    BOOL ok = SUCCEEDED(t->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE)); t->Release();
    return ok;
}
// Does the task start this very exe? (a portable copy must not launch the installed one)
static BOOL TaskTargets(IRegisteredTask* t, const WCHAR* exe) {
    BSTR x = NULL; BOOL ok = FALSE;
    if (SUCCEEDED(t->get_Xml(&x)) && x) {
        WCHAR ee[MAX_PATH * 2]; XmlEsc(ee, MAX_PATH * 2, exe);
        size_t n = wcslen(ee);
        for (const WCHAR* p = x; *p && !ok; p++) if (!_wcsnicmp(p, ee, n)) ok = TRUE;
        SysFreeString(x);
    }
    return ok;
}
static BOOL TaskRunFor(const WCHAR* name, const WCHAR* exe) {
    IRegisteredTask* t = TsGet(name); if (!t) return FALSE;
    BOOL ok = FALSE;
    if (TaskTargets(t, exe)) {
        VARIANT v; VariantInit(&v); IRunningTask* r = NULL;
        ok = SUCCEEDED(t->Run(v, &r));
        if (r) r->Release();
    }
    t->Release();
    return ok;
}
