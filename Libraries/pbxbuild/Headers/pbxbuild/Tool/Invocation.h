/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#ifndef __pbxbuild_Tool_Invocation_h
#define __pbxbuild_Tool_Invocation_h

#include <pbxbuild/Base.h>
#include <dependency/DependencyInfoFormat.h>

namespace pbxbuild {
namespace Tool {

class Invocation {
public:
    class AuxiliaryFile {
    private:
        std::string                         _path;
        ext::optional<std::vector<uint8_t>> _contentsData;
        ext::optional<std::string>          _contentsPath;
        bool                                _executable;

    public:
        AuxiliaryFile(std::string const &path, std::vector<uint8_t> const &contentsData, bool executable);
        AuxiliaryFile(std::string const &path, std::string const &contentsPath, bool executable);
        ~AuxiliaryFile();

    public:
        std::string const &path() const
        { return _path; }
        ext::optional<std::vector<uint8_t>> const &contentsData() const
        { return _contentsData; }
        ext::optional<std::string> const &contentsPath() const
        { return _contentsPath; }
        bool executable() const
        { return _executable; }
    };

public:
    class DependencyInfo {
    private:
        dependency::DependencyInfoFormat _format;
        std::string                      _path;

    public:
        DependencyInfo(dependency::DependencyInfoFormat format, std::string const &path);

    public:
        dependency::DependencyInfoFormat format() const
        { return _format; }
        std::string const &path() const
        { return _path; }
    };

public:
    /*
     * Represents the executable used to run the tool for the invocation.
     */
    class Executable {
    private:
        std::string _path;
        std::string _builtin;

    public:
        Executable(std::string const &path, std::string const &builtin);

    public:
        /*
         * The path to the executable. If this executable is a bulitin tool, points
         * to a standalone executable verison of that tool.
         */
        std::string const &path() const
        { return _path; }

        /*
         * The name of the builtin tool this executable corresponds to.
         */
        std::string const &builtin() const
        { return _builtin; }

    public:
        /*
         * The user-facing name to show for the executable. For builtin tools, this
         * uses the shorter name of the bulitin tool rather than the filesystem path.
         */
        std::string const &displayName() const;

    public:
        /*
         * Creates an executable from an unknown string. The executable could
         * be a builtin tool (starts with "builtin-"), an absolute path to a tool,
         * or a relative path to search in the executable paths.
         */
        static Executable
        Determine(std::string const &executable, std::vector<std::string> const &executablePaths);

        /*
         * Creates an executable with a known absolute path.
         */
        static Executable
        Absolute(std::string const &path);

        /*
         * Creates an executable for a known built-in tool.
         */
        static Executable
        Builtin(std::string const &name);
    };

private:
    Executable                                   _executable;
    std::vector<std::string>                     _arguments;
    std::unordered_map<std::string, std::string> _environment;
    std::string                                  _workingDirectory;

private:
    std::vector<std::string>                     _inputs;
    std::vector<std::string>                     _outputs;
    std::vector<std::string>                     _phonyInputs;

private:
    std::vector<std::string>                     _inputDependencies;
    std::vector<std::string>                     _orderDependencies;

private:
    std::vector<DependencyInfo>                  _dependencyInfo;
    std::vector<AuxiliaryFile>                   _auxiliaryFiles;

private:
    std::string                                  _logMessage;
    bool                                         _showEnvironmentInLog;

private:
    bool                                         _createsProductStructure;

public:
    Invocation();
    ~Invocation();

public:
    Executable const &executable() const
    { return _executable; }
    std::vector<std::string> const &arguments() const
    { return _arguments; }
    std::unordered_map<std::string, std::string> const &environment() const
    { return _environment; }
    std::string const &workingDirectory() const
    { return _workingDirectory; }

public:
    Executable &executable()
    { return _executable; }
    std::vector<std::string> &arguments()
    { return _arguments; }
    std::unordered_map<std::string, std::string> &environment()
    { return _environment; }
    std::string &workingDirectory()
    { return _workingDirectory; }

public:
    std::vector<std::string> const &inputs() const
    { return _inputs; }
    std::vector<std::string> const &outputs() const
    { return _outputs; }

public:
    /* Inputs that may not exist or be generated by an invocation. */
    std::vector<std::string> const &phonyInputs() const
    { return _phonyInputs; }

public:
    std::vector<std::string> &inputs()
    { return _inputs; }
    std::vector<std::string> &outputs()
    { return _outputs; }

public:
    std::vector<std::string> &phonyInputs()
    { return _phonyInputs; }

public:
    std::vector<std::string> const &inputDependencies() const
    { return _inputDependencies; }
    std::vector<std::string> const &orderDependencies() const
    { return _orderDependencies; }

public:
    std::vector<std::string> &inputDependencies()
    { return _inputDependencies; }
    std::vector<std::string> &orderDependencies()
    { return _orderDependencies; }

public:
    std::vector<DependencyInfo> const &dependencyInfo() const
    { return _dependencyInfo; }
    std::vector<AuxiliaryFile> const &auxiliaryFiles() const
    { return _auxiliaryFiles; }

public:
    std::vector<DependencyInfo> &dependencyInfo()
    { return _dependencyInfo; }
    std::vector<AuxiliaryFile> &auxiliaryFiles()
    { return _auxiliaryFiles; }

public:
    std::string const &logMessage() const
    { return _logMessage; }
    bool showEnvironmentInLog() const
    { return _showEnvironmentInLog; }

public:
    std::string &logMessage()
    { return _logMessage; }
    bool &showEnvironmentInLog()
    { return _showEnvironmentInLog; }

public:
    bool createsProductStructure() const
    { return _createsProductStructure; }

public:
    bool &createsProductStructure()
    { return _createsProductStructure; }
};

}
}

#endif // !__pbxbuild_Tool_Invocation_h
