using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// General Information about an assembly is controlled through the following
// set of attributes. Change these attribute values to modify the information
// associated with an assembly.
[assembly: AssemblyTitle("SilentHillPC_Launcher")]
[assembly: AssemblyDescription("")]
[assembly: AssemblyConfiguration("")]
[assembly: AssemblyCompany("")]
[assembly: AssemblyProduct("SilentHillPC_Launcher")]
[assembly: AssemblyCopyright("Copyright ©  2026")]
[assembly: AssemblyTrademark("")]
[assembly: AssemblyCulture("")]

// Setting ComVisible to false makes the types in this assembly not visible
// to COM components.  If you need to access a type in this assembly from
// COM, set the ComVisible attribute to true on that type.
[assembly: ComVisible(false)]

// The following GUID is for the ID of the typelib if this project is exposed to COM
[assembly: Guid("e4f67033-83af-4dae-9dce-5cc62e6b1f6d")]

// Version information for an assembly consists of the following four values:
//
//      Major Version
//      Minor Version
//      Build Number
//      Revision
//
// AssemblyFileVersion is date-based (yyyy.M.d.rev) and is the ordering used to
// decide launcher self-updates: the launcher only replaces itself when an
// incoming build's launcher_version is STRICTLY GREATER than this. BUMP THIS
// every time the launcher exe changes (and the release will carry it in
// version.json), otherwise downgrades/no-ops can't be told apart. AssemblyVersion
// stays 1.0.0.0 so it isn't a binding identity churn.
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("2026.6.22.1")]
