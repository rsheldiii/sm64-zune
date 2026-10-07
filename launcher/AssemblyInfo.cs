// Identity of the launcher assembly. The GUID is the app identity on the Zune and must match
// launcher/application.cfg.
//
// exploiter.exe is these four files as Visual Studio 2008 with XNA Game Studio 3.1 built them
// (Release, Zune). It is kept as built: the launcher depends on exactly how its code is
// compiled, and one compiled by a current C# compiler does not start the game.
using System.Reflection;
using System.Runtime.InteropServices;

[assembly: AssemblyTitle("Super Mario 64")]
[assembly: AssemblyVersion("0.1.0.*")]
[assembly: Guid("eca37a3a-b901-4b67-b185-c1b244386b32")]
