# check_uiaccess.ps1 - Show whether each running mtpagent process has UIAccess (read-only; changes nothing)
#
# Expected after tools\manual-install\install.ps1:
#   path = C:\Program Files\MTP\mtpagent.exe, uiAccess = True, elevated = False
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Tok {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool GetTokenInformation(IntPtr token, int cls, out int value, int len, out int ret);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr buf, int len, out int ret);
  [DllImport("advapi32.dll")] public static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
  [DllImport("advapi32.dll")] public static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, TOKEN_QUERY = 0x0008;
  const int TokenElevation = 20, TokenIntegrityLevel = 25, TokenUIAccess = 26;

  public static string Describe(int pid) {
    IntPtr p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
    if (p == IntPtr.Zero) return "cannot open process (error " + Marshal.GetLastWin32Error() + ")";
    IntPtr t;
    if (!OpenProcessToken(p, TOKEN_QUERY, out t)) { CloseHandle(p); return "cannot open token (error " + Marshal.GetLastWin32Error() + ")"; }
    int ui, elev, ret;
    GetTokenInformation(t, TokenUIAccess, out ui, 4, out ret);
    GetTokenInformation(t, TokenElevation, out elev, 4, out ret);
    string il = "?";
    IntPtr buf = Marshal.AllocHGlobal(256);
    if (GetTokenInformation(t, TokenIntegrityLevel, buf, 256, out ret)) {
      IntPtr sid = Marshal.ReadIntPtr(buf);
      int n = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
      int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(n - 1)));
      il = rid >= 0x4000 ? "System" : rid >= 0x3000 ? "High" : rid >= 0x2000 ? "Medium" : "Low";
      il += string.Format(" (0x{0:X})", rid);
    }
    Marshal.FreeHGlobal(buf);
    CloseHandle(t); CloseHandle(p);
    return "uiAccess = " + (ui != 0) + ", elevated = " + (elev != 0) + ", integrity = " + il;
  }
}
"@
$procs = Get-Process mtpagent -ErrorAction SilentlyContinue
if (-not $procs) { "mtpagent is not running"; exit 1 }
foreach ($p in $procs) {
    "pid $($p.Id): path = $($p.Path)"
    "    " + [Tok]::Describe($p.Id)
}
