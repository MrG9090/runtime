// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;

namespace Microsoft.DotNet.CoreSetup.Test
{
    public static class Constants
    {
        public const string MicrosoftNETCoreApp = "Microsoft.NETCore.App";

        public static class ApplyPatchesSetting
        {
            public const string RuntimeConfigPropertyName = "applyPatches";
        }

        public static class RollForwardOnNoCandidateFxSetting
        {
            public const string RuntimeConfigPropertyName = "rollForwardOnNoCandidateFx";
            public const string CommandLineArgument = "--roll-forward-on-no-candidate-fx";
            public const string EnvironmentVariable = "DOTNET_ROLL_FORWARD_ON_NO_CANDIDATE_FX";
        }

        public static class RollForwardSetting
        {
            public const string RuntimeConfigPropertyName = "rollForward";
            public const string CommandLineArgument = "--roll-forward";
            public const string EnvironmentVariable = "DOTNET_ROLL_FORWARD";

            public const string LatestPatch = "LatestPatch";
            public const string Minor = "Minor";
            public const string Major = "Major";
            public const string LatestMinor = "LatestMinor";
            public const string LatestMajor = "LatestMajor";
            public const string Disable = "Disable";
        }

        public static class FxVersion
        {
            public const string CommandLineArgument = "--fx-version";
        }

        public static class AdditionalDeps
        {
            public const string CommandLineArgument = "--additional-deps";
        }

        public static class AdditionalProbingPath
        {
            public const string CommandLineArgument = "--additionalprobingpath";
            public const string RuntimeConfigPropertyName = "additionalProbingPaths";
        }

        public static class Breadcrumbs
        {
            public const string EnvironmentVariable = "CORE_BREADCRUMBS";
        }

        public static class BundleExtractBase
        {
            public const string EnvironmentVariable = "DOTNET_BUNDLE_EXTRACT_BASE_DIR";
        }

        public static class CoreServicing
        {
            public const string EnvironmentVariable = "CORE_SERVICING";
        }

        public static class DepsFile
        {
            public const string CommandLineArgument = "--depsfile";
        }

        public static class RollForwardToPreRelease
        {
            public const string EnvironmentVariable = "DOTNET_ROLL_FORWARD_TO_PRERELEASE";
        }

        public static class DisableGuiErrors
        {
            public const string EnvironmentVariable = "DOTNET_DISABLE_GUI_ERRORS";
        }

        public static class TestOnlyEnvironmentVariables
        {
            public const string DefaultInstallPath = "_DOTNET_TEST_DEFAULT_INSTALL_PATH";
            public const string GloballyRegisteredPath = "_DOTNET_TEST_GLOBALLY_REGISTERED_PATH";

            public static string RegisteredConfigLocation = OperatingSystem.IsWindows() ? RegistryPath : InstallLocationPath;
            public const string RegistryPath = "_DOTNET_TEST_REGISTRY_PATH";
            public const string InstallLocationPath = "_DOTNET_TEST_INSTALL_LOCATION_PATH";
        }

        public static class RuntimeId
        {
            public const string EnvironmentVariable = "DOTNET_RUNTIME_ID";
        }

        public static class MultilevelLookup
        {
            public const string EnvironmentVariable = "DOTNET_MULTILEVEL_LOOKUP";
        }

        public static class HostTracing
        {
            public const string TraceLevelEnvironmentVariable = "DOTNET_HOST_TRACE";
            public const string TraceFileEnvironmentVariable = "DOTNET_HOST_TRACEFILE";
            public const string VerbosityEnvironmentVariable = "DOTNET_HOST_TRACE_VERBOSITY";
        }

        public static class DotnetRoot
        {
            public const string EnvironmentVariable = "DOTNET_ROOT";
            public const string WindowsX86EnvironmentVariable = "DOTNET_ROOT(x86)";
            public const string ArchitectureEnvironmentVariablePrefix = "DOTNET_ROOT_";
        }

        public static class ErrorCode
        {
            public const int Success = 0;
            public const int InvalidArgFailure = unchecked((int)0x80008081);
            public const int CoreHostLibMissingFailure = unchecked((int)0x80008083);
            public const int EntryPointFailure = unchecked((int)0x80008084);
            public const int ResolverInitFailure = unchecked((int)0x8000808b);
            public const int ResolverResolveFailure = unchecked((int)0x8000808c);
            public const int LibHostInvalidArgs = unchecked((int)0x80008092);
            public const int InvalidConfigFile = unchecked((int)0x80008093);
            public const int AppArgNotRunnable = unchecked((int)0x80008094);
            public const int AppHostExeNotBoundFailure = unchecked((int)0x80008095);
            public const int FrameworkMissingFailure = unchecked((int)0x80008096);
            public const int FrameworkCompatFailure = unchecked((int)0x8000809c);
            public const int BundleExtractionFailure = unchecked((int)0x8000809f);

            public const int COMPlusException = unchecked((int)0xe0434352);
            public const int SIGABRT = 134;
        }
    }
}
