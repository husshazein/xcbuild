/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <xcexecution/NinjaExecutor.h>

#include <xcexecution/Parameters.h>
#include <pbxbuild/Phase/Environment.h>
#include <pbxbuild/Phase/PhaseInvocations.h>
#include <ninja/Writer.h>
#include <ninja/Value.h>
#include <libutil/Escape.h>
#include <libutil/Filesystem.h>
#include <libutil/FSUtil.h>
#include <libutil/Subprocess.h>
#include <libutil/SysUtil.h>
#include <libutil/md5.h>

#include <sstream>
#include <iomanip>

#include <sys/types.h>
#include <sys/stat.h>

using xcexecution::NinjaExecutor;
using xcexecution::Parameters;
using libutil::Escape;
using libutil::Filesystem;
using libutil::FSUtil;
using libutil::Subprocess;
using libutil::SysUtil;

NinjaExecutor::
NinjaExecutor(std::shared_ptr<xcformatter::Formatter> const &formatter, bool dryRun, bool generate) :
    Executor(formatter, dryRun, generate)
{
}

NinjaExecutor::
~NinjaExecutor()
{
}

static std::string
TargetNinjaBegin(pbxproj::PBX::Target::shared_ptr const &target)
{
    return "begin-target-" + target->name();
}

static std::string
TargetNinjaFinish(pbxproj::PBX::Target::shared_ptr const &target)
{
    return "finish-target-" + target->name();
}

static std::string
TargetNinjaPath(pbxproj::PBX::Target::shared_ptr const &target, pbxbuild::Target::Environment const &targetEnvironment)
{
    /*
     * Determine where the Ninja file should go. We use the target's temp dir
     * as, being target-specific, it will allow the Ninja files to not conflict.
     */
    pbxsetting::Environment const &environment = targetEnvironment.environment();
    std::string temporaryDirectory = environment.resolve("TARGET_TEMP_DIR");
    // TODO(grp): How to handle varying configurations / actions / other build context options?

    return temporaryDirectory + "/" + "build.ninja";
}

static std::string
NinjaRuleName()
{
    return "invoke";
}

static std::string
NinjaDescription(std::string const &description)
{
    /* Limit to the first line: Ninja can only handle a single line status. */
    std::string::size_type newline = description.find('\n');
    if (newline != std::string::npos) {
        return description.substr(0, description.find('\n'));
    } else {
        return description;
    }
}

static std::string
NinjaHash(std::string const &input)
{
    md5_state_t state;
    md5_init(&state);
    md5_append(&state, reinterpret_cast<const md5_byte_t *>(input.data()), input.size());
    uint8_t digest[16];
    md5_finish(&state, reinterpret_cast<md5_byte_t *>(&digest));

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t c : digest) {
        ss << std::setw(2) << static_cast<int>(c);
    }

    return ss.str();
}

static std::string
NinjaInvocationPhonyOutput(pbxbuild::Tool::Invocation const &invocation)
{
    /*
     * This is a hack to support invocations that have no outputs. Ninja requires
     * all targets to have an output, but in some cases (usually with build script
     * invocations), the invocation has no outputs.
     *
     * In that case, generate a phony output name that will never exist, so the
     * target will always be out of date. This is expected, since with no outputs
     * there's no way to tell if the invocation needs to run.
     */

    // TODO(grp): Handle identical phony output invocations in a build.
    std::string key = invocation.executable().path();
    for (std::string const &arg : invocation.arguments()) {
        key += " " + arg;
    }

    return ".ninja-phony-output-" + NinjaHash(key);
}

static std::vector<std::string>
NinjaInvocationOutputs(pbxbuild::Tool::Invocation const &invocation)
{
    std::vector<std::string> outputs;

    if (!invocation.outputs().empty()) {
        outputs.insert(outputs.end(), invocation.outputs().begin(), invocation.outputs().end());
    } else {
        outputs.push_back(NinjaInvocationPhonyOutput(invocation));
    }

    return outputs;
}

static void
WriteNinjaRegenerate(ninja::Writer *writer, Parameters const &buildParameters, std::string const &ninjaPath, std::string const &configurationHashPath, std::vector<std::string> const &inputPaths)
{
    /*
     * Regenerate using this executor. Force regeneration to avoid recursively
     * executing Ninja when Ninja itself calls this generate command.
     */
    std::vector<std::string> generateArguments = { "-generate", "-executor", "ninja" };

    /*
     * Add arguments necessary to recreate the same set of build parameters.
     */
    std::vector<std::string> parameterArguments = buildParameters.canonicalArguments();
    generateArguments.insert(generateArguments.end(), parameterArguments.begin(), parameterArguments.end());

    /*
     * Re-run the current executable to re-generate the Ninja file. There is an
     * implicit assumption here that this executable takes the parameters above.
     */
    std::string exec = Escape::Shell(SysUtil::GetExecutablePath());

    /*
     * Escape executable and input parameters for Ninja.
     */
    for (std::string const &arg : generateArguments) {
        exec += " " + Escape::Shell(arg);
    }
    std::vector<ninja::Value> inputPathValues;
    inputPathValues.push_back(ninja::Value::String(configurationHashPath));
    for (std::string const &inputPath : inputPaths) {
        inputPathValues.push_back(ninja::Value::String(inputPath));
    }

    /*
     * Write out the Ninja rule to regenerate sources.
     */
    std::string ruleName = "regenerate";
    writer->rule(ruleName, ninja::Value::Expression("cd $dir && $exec"));
    writer->build({ ninja::Value::String(ninjaPath) }, ruleName, inputPathValues, {
        { "dir", ninja::Value::String(Escape::Shell(FSUtil::GetCurrentDirectory())) },
        { "exec", ninja::Value::String(exec) },
        { "description", ninja::Value::String("Regenerating Ninja files...") },

        /* This command regenerates the Ninja files. */
        { "generator", ninja::Value::String("1") },

        /* Use the console pool to pass through terminal settings. */
        { "pool", ninja::Value::String("console") },
    });
}

static bool
WriteNinja(Filesystem *filesystem, ninja::Writer const &writer, std::string const &path)
{
    if (!filesystem->createDirectory(FSUtil::GetDirectoryName(path))) {
        return false;
    }

    std::string contents = writer.serialize();
    std::vector<uint8_t> copy = std::vector<uint8_t>(contents.begin(), contents.end());
    if (!filesystem->write(copy, path)) {
        return false;
    }

    return true;
}

static bool
ShouldGenerateNinja(Filesystem const *filesystem, bool generate, Parameters const &buildParameters, std::string const &ninjaPath, std::string const &configurationHashPath)
{
    /*
     * If explicitly asked to generate, definitely need to regenerate.
     */
    if (generate) {
        return true;
    }

    /*
     * If the Ninja file doesn't exist, must re-generate to create it.
     */
    if (!filesystem->exists(ninjaPath)) {
        return true;
    }

    /*
     * If the configuration hash doesn't exist, the configuration is unknown so
     * the Ninja must be regenerated to ensure it matches the configuration.
     */
    if (!filesystem->exists(configurationHashPath)) {
        return true;
    }

    /*
     * If the contents of the configration hash doesn't match, need to update for
     * the new configuration.
     */
    std::vector<uint8_t> contents;
    if (!filesystem->read(&contents, configurationHashPath)) {
        /* Can't be read, same as not existing. */
        return true;
    }
    if (std::string(contents.begin(), contents.end()) != buildParameters.canonicalHash()) {
        return true;
    }

    /*
     * Nothing changed, safe to use the cached Ninja.
     */
    return false;
}

bool NinjaExecutor::
build(
    Filesystem *filesystem,
    pbxbuild::Build::Environment const &buildEnvironment,
    Parameters const &buildParameters)
{
    /*
     * Load the derived data hash in order to output the Ninja file in the
     * right derived data directory. This does not load the workspace context
     * to avoid loading potentially very large projects for incremental builds.
     */
    std::string workspacePath = filesystem->resolvePath(buildParameters.workspace().value_or(*buildParameters.project()));
    pbxbuild::DerivedDataHash derivedDataHash = pbxbuild::DerivedDataHash::Create(workspacePath);
    pbxsetting::Level derivedDataLevel = pbxsetting::Level(derivedDataHash.overrideSettings());

    /*
     * This environment contains only settings shared for the entire build.
     */
    pbxsetting::Environment environment = buildEnvironment.baseEnvironment();
    environment.insertFront(derivedDataLevel, false);

    /*
     * Determine where build-level outputs will go. Note we can't use CONFIGURATION_BUILD_DIR
     * at this point because that includes the EFFECTIVE_PLATFORM_NAME, but we don't have a platform.
     */
    std::string intermediatesDirectory = environment.resolve("OBJROOT");
    std::string ninjaPath = intermediatesDirectory + "/" + "build.ninja";
    std::string configurationHashPath = intermediatesDirectory + "/" + ".ninja-configuration";

    /*
     * If the Ninja file needs to be generated, generate it.
     */
    if (ShouldGenerateNinja(filesystem, _generate, buildParameters, ninjaPath, configurationHashPath)) {
        fprintf(stderr, "Generating Ninja files...\n");

        /*
         * Load the workspace. This can be quite slow, so only do it if it's needed to generate
         * the Ninja file. Similarly, only resolve dependencies in that case.
         */
        ext::optional<pbxbuild::WorkspaceContext> workspaceContext = buildParameters.loadWorkspace(filesystem, buildEnvironment, FSUtil::GetCurrentDirectory());
        if (!workspaceContext) {
            fprintf(stderr, "error: unable to load workspace\n");
            return false;
        }

        ext::optional<pbxbuild::Build::Context> buildContext = buildParameters.createBuildContext(*workspaceContext);
        if (!buildContext) {
            fprintf(stderr, "error: unable to create build context\n");
            return false;
        }

        ext::optional<pbxbuild::DirectedGraph<pbxproj::PBX::Target::shared_ptr>> targetGraph = buildParameters.resolveDependencies(buildEnvironment, *buildContext);
        if (!targetGraph) {
            fprintf(stderr, "error: unable to resolve dependencies\n");
            return false;
        }

        /*
         * Generate the Ninja file.
         */
        bool result = buildAction(
            filesystem,
            buildParameters,
            buildEnvironment,
            *buildContext,
            *targetGraph,
            ninjaPath,
            configurationHashPath,
            intermediatesDirectory);

        if (!result) {
            fprintf(stderr, "error: failed to generate build.ninja\n");
            return false;
        }

        /*
         * Write out the configuration hash for the parameters in the Ninja.
         */
        std::string hashContents = buildParameters.canonicalHash();
        auto contents = std::vector<uint8_t>(hashContents.begin(), hashContents.end());
        if (!filesystem->write(contents, configurationHashPath)) {
            fprintf(stderr, "error: failed to generate ninja configuration hash\n");
            return false;
        }
    }

    /*
     * Only perform a build if not passing -generate. If -generate is passed, that's because Ninja
     * is already running and asking to re-generate the project file. Re-running it would recurse.
     */
    if (!_generate) {
        /*
         * Use the Ninja file just generated.
         */
        std::vector<std::string> arguments = { "-f", ninjaPath };

        /*
         * Find the path to the Ninja executable to use.
         */
        ext::optional<std::string> executable = filesystem->findExecutable("ninja", FSUtil::GetExecutablePaths());
        if (!executable) {
            /*
             * Couldn't find standard Ninja, try with llbuild.
             */
            executable = filesystem->findExecutable("llbuild", FSUtil::GetExecutablePaths());

            /*
             * If neither Ninja or llbuild are available, can't start the build.
             */
            if (!executable) {
                fprintf(stderr, "error: could not find ninja or llbuild in PATH\n");
                return false;
            }

            /*
             * Use llbuild's Ninja executor, which requires extra arguments.
             */
            std::vector<std::string> llbuildArguments = { "ninja", "build" };
            arguments.insert(arguments.begin(), llbuildArguments.begin(), llbuildArguments.end());
        }

        /*
         * Pass through the dry run option.
         */
        if (_dryRun) {
            arguments.push_back("-n");
        }

        // TODO(grp): Pass number of jobs if specified.

        /*
         * Pass through all environment variables, in case they affect Ninja or build settings
         * when re-generating the Ninja files.
         */
        std::unordered_map<std::string, std::string> environmentVariables = SysUtil::EnvironmentVariables();

        /*
         * Run Ninja and return if it failed. Ninja itself does the build.
         */
        Subprocess ninja;
        if (!ninja.execute(*executable, arguments, environmentVariables, intermediatesDirectory) || ninja.exitcode() != 0) {
            return false;
        }
    }

    return true;
}

bool NinjaExecutor::
buildAction(
    Filesystem *filesystem,
    Parameters const &buildParameters,
    pbxbuild::Build::Environment const &buildEnvironment,
    pbxbuild::Build::Context const &buildContext,
    pbxbuild::DirectedGraph<pbxproj::PBX::Target::shared_ptr> const &targetGraph,
    std::string const &ninjaPath,
    std::string const &configurationHashPath,
    std::string const &intermediatesDirectory)
{
    /*
     * Write out a Ninja file for the build as a whole. Note each target will have a separate
     * file, this is to coordinate the build between targets.
     */
    ninja::Writer writer;
    writer.comment("xcbuild ninja");
    writer.comment("Action: " + buildContext.action());
    if (buildContext.workspaceContext().workspace() != nullptr) {
        writer.comment("Workspace: " + buildContext.workspaceContext().workspace()->projectFile());
    } else if (buildContext.workspaceContext().project() != nullptr) {
        writer.comment("Project: " + buildContext.workspaceContext().project()->projectFile());
    }
    if (buildContext.scheme() != nullptr) {
        writer.comment("Scheme: " + buildContext.scheme()->name());
    }
    writer.comment("Configuation: " + buildContext.configuration());
    writer.newline();

    /*
     * Ninja's intermediate outputs should also go in the temp dir.
     */
    writer.binding({ "builddir", { ninja::Value::String(intermediatesDirectory) } });
    writer.newline();

    /*
     * Since invocations are already resolved at this point, we can't use more specific
     * rules at the Ninja level. Instead, add a single rule that just passes through from
     * the build command that calls it.
     */
    writer.rule(NinjaRuleName(), ninja::Value::Expression("cd $dir && env -i $env $exec && $depexec"));

    /*
     * Build up a list of all of the inputs to the build, so Ninja can regenerate as necessary.
     */
    std::vector<std::string> inputPaths = buildContext.workspaceContext().loadedFilePaths();

    /*
     * Go over each target and write out Ninja targets for the start and end of each.
     * Don't bother topologically sorting the targets now, since Ninja will do that for us.
     */
    for (pbxproj::PBX::Target::shared_ptr const &target : targetGraph.nodes()) {

        /*
         * Beginning target depends on finishing the targets before that. This is implemented
         * in three parts:
         *
         *  1. Each target has a "target begin" Ninja target depending on completing the build
         *     of any dependent targets.
         *  2. Each invocation's Ninja target depends on the "target begin" target to order
         *     them necessarily after the target started building.
         *  3. Each target also has a "target finish" Ninja target, which depends on all of
         *     the invocations created for the target.
         *
         * The end result is that targets build in the right order. Note this does not preclude
         * cross-target parallelization; if the target dependency graph doesn't have an edge,
         * then they will be parallelized. Linear builds have edges from each target to all
         * previous targets.
         */

        /*
         * Resolve this target and generate its invocations.
         */
        ext::optional<pbxbuild::Target::Environment> targetEnvironment = buildContext.targetEnvironment(buildEnvironment, target);
        if (!targetEnvironment) {
            fprintf(stderr, "error: couldn't create target environment for %s\n", target->name().c_str());
            continue;
        }

        pbxbuild::Phase::Environment phaseEnvironment = pbxbuild::Phase::Environment(buildEnvironment, buildContext, target, *targetEnvironment);
        pbxbuild::Phase::PhaseInvocations phaseInvocations = pbxbuild::Phase::PhaseInvocations::Create(phaseEnvironment, target);

        /*
         * As described above, the target's begin depends on all of the target dependencies.
         */
        std::vector<ninja::Value> dependenciesFinished;
        for (pbxproj::PBX::Target::shared_ptr const &dependency : targetGraph.adjacent(target)) {
            std::string targetFinished = TargetNinjaFinish(dependency);
            dependenciesFinished.push_back(ninja::Value::String(targetFinished));
        }

        /*
         * Add the phony target for beginning this target's build.
         */
        std::string targetBegin = TargetNinjaBegin(target);
        writer.build({ ninja::Value::String(targetBegin) }, "phony", dependenciesFinished);

        /*
         * Write out the Ninja file to build this target.
         */
        if (!buildTargetInvocations(filesystem, target, *targetEnvironment, phaseInvocations.invocations())) {
            fprintf(stderr, "error: failed to build target ninja\n");
            return false;
        }

        /*
         * Load the Ninja file generated for this target.
         */
        std::string targetPath = TargetNinjaPath(target, *targetEnvironment);
        writer.subninja(ninja::Value::String(targetPath));

        /*
         * As described above, the target's finish depends on all of the invocation outputs.
         */
        std::unordered_set<std::string> invocationOutputs;
        for (pbxbuild::Tool::Invocation const &invocation : phaseInvocations.invocations()) {
            if (invocation.executable().path().empty()) {
                /* No outputs. */
                continue;
            }

            std::vector<std::string> outputs = NinjaInvocationOutputs(invocation);
            invocationOutputs.insert(outputs.begin(), outputs.end());
        }

        /*
         * Add phony rules for input dependencies that we don't know if they exist.
         * This can come up, for example, for user-specified custom script inputs.
         * However, avoid adding the phony invocation if a real output *does* include
         * the phony input, to avoid Ninja complaining about duplicate rules.
         */
        for (pbxbuild::Tool::Invocation const &invocation : phaseInvocations.invocations()) {
            for (std::string const &phonyInput : invocation.phonyInputs()) {
                if (invocationOutputs.find(phonyInput) == invocationOutputs.end()) {
                    writer.build({ ninja::Value::String(phonyInput) }, "phony", { });
                }
            }
        }

        /*
         * Add the phony target for ending this target's build.
         */
        std::string targetFinish = TargetNinjaFinish(target);
        std::vector<ninja::Value> invocationOutputsValues;
        for (std::string const &output : invocationOutputs) {
            invocationOutputsValues.push_back(ninja::Value::String(output));
        }
        writer.build({ ninja::Value::String(targetFinish) }, "phony", { }, { }, invocationOutputsValues);

        /*
         * Add loaded files for the target configuration to the inputs.
         */
        if (targetEnvironment->projectConfigurationFile() != nullptr) {
            inputPaths.push_back(targetEnvironment->projectConfigurationFile()->path());
        }
        if (targetEnvironment->targetConfigurationFile() != nullptr) {
            inputPaths.push_back(targetEnvironment->targetConfigurationFile()->path());
        }
    }

    /*
     * Add a Ninja rule to regenerate the build.ninja file itself.
     */
    WriteNinjaRegenerate(&writer, buildParameters, ninjaPath, configurationHashPath, inputPaths);

    /*
     * Serialize the Ninja file into the build root.
     */
    if (!WriteNinja(filesystem, writer, ninjaPath)) {
        fprintf(stderr, "error: failed to write Ninja to %s\n", ninjaPath.c_str());
        return false;
    }

    /*
     * Note where the Ninja file is written.
     */
    fprintf(stderr, "Wrote Ninja: %s\n", ninjaPath.c_str());

    return true;
}

static std::string
LocalExecutable(std::string const &executable)
{
    std::string executableRoot = FSUtil::GetDirectoryName(SysUtil::GetExecutablePath());
    return executableRoot + "/" + executable;
}

static std::string
NinjaDependencyInfoExecutable()
{
    return LocalExecutable("dependency-info-tool");
}

bool NinjaExecutor::
buildTargetAuxiliaryFiles(
    Filesystem *filesystem,
    ninja::Writer *writer,
    pbxproj::PBX::Target::shared_ptr const &target,
    pbxbuild::Target::Environment const &targetEnvironment,
    std::vector<pbxbuild::Tool::Invocation> const &invocations)
{
    // TODO(grp): Could this defer writing auxiliary files and let Ninja do it?
    for (pbxbuild::Tool::Invocation const &invocation : invocations) {
        for (pbxbuild::Tool::Invocation::AuxiliaryFile const &auxiliaryFile : invocation.auxiliaryFiles()) {
            std::string auxiliaryDirectory = FSUtil::GetDirectoryName(auxiliaryFile.path());
            if (!filesystem->createDirectory(auxiliaryDirectory)) {
                fprintf(stderr, "error: failed to create auxiliary directory: %s\n", auxiliaryDirectory.c_str());
                return false;
            }

            if (auxiliaryFile.contentsData()) {
                if (!filesystem->write(*auxiliaryFile.contentsData(), auxiliaryFile.path())) {
                    fprintf(stderr, "error: failed to write auxiliary file: %s\n", auxiliaryFile.path().c_str());
                    return false;
                }
            } else if (auxiliaryFile.contentsPath()) {
                std::vector<uint8_t> contents;
                if (!filesystem->read(&contents, *auxiliaryFile.contentsPath())) {
                    fprintf(stderr, "error: failed to read auxiliary file contents: %s\n", auxiliaryFile.contentsPath()->c_str());
                    return false;
                }
                if (!filesystem->write(contents, auxiliaryFile.path())) {
                    fprintf(stderr, "error: failed to write auxiliary file: %s\n", auxiliaryFile.path().c_str());
                    return false;
                }
            }

            if (auxiliaryFile.executable() && !filesystem->isExecutable(auxiliaryFile.path())) {
                // FIXME: Use the filesystem for this.
                if (::chmod(auxiliaryFile.path().c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
                    fprintf(stderr, "error: failed to mark auxiliary file executable: %s\n", auxiliaryFile.path().c_str());
                    return false;
                }
            }
        }
    }

    return true;
}

bool NinjaExecutor::
buildTargetInvocations(
    Filesystem *filesystem,
    pbxproj::PBX::Target::shared_ptr const &target,
    pbxbuild::Target::Environment const &targetEnvironment,
    std::vector<pbxbuild::Tool::Invocation> const &invocations)
{
    std::string targetBegin = TargetNinjaBegin(target);

    /*
     * Start building the Ninja file for this target.
     */
    ninja::Writer writer;
    writer.comment("xcbuild ninja");
    writer.comment("Target: " + target->name());
    writer.newline();

    /*
     * Write out auxiliary files used by the invocations.
     */
    if (!buildTargetAuxiliaryFiles(filesystem, &writer, target, targetEnvironment, invocations)) {
        return false;
    }

    /*
     * Add the build command for each invocation.
     */
    for (pbxbuild::Tool::Invocation const &invocation : invocations) {
        // TODO(grp): This should perhaps be a separate flag for a 'phony' invocation.
        if (invocation.executable().path().empty()) {
            continue;
        }

        /*
         * Build the invocation arguments. Must escape for shell arguments as Ninja passes
         * the command string directly to the shell, which would interpret spaces, etc as meaningful.
         */
        std::string exec = Escape::Shell(invocation.executable().path());
        for (std::string const &arg : invocation.arguments()) {
            exec += " " + Escape::Shell(arg);
        }

        /*
         * Build the invocation environment. To set the environment, we use standard shell syntax.
         * Use `env` to avoid Bash-specific limitations on environment variables. Specifically, some
         * versions of Bash don't allow setting "UID". Pass -i to clear out the environment.
         */
        std::string environment;
        for (auto it = invocation.environment().begin(); it != invocation.environment().end(); ++it) {
            if (it != invocation.environment().begin()) {
                environment += " ";
            }
            environment += it->first + "=" + Escape::Shell(it->second);
        }

        /*
         * Determine the status message for Ninja to print for this invocation.
         */
        std::string description = NinjaDescription(_formatter->beginInvocation(invocation, invocation.executable().displayName(), false));

        /*
         * Add the dependency info converter & file.
         */
        std::string dependencyInfoFile;
        std::string dependencyInfoExec;

        if (!invocation.dependencyInfo().empty()) {
            /* Determine the first output; Ninja expects that as the Makefile rule. */
            std::string output = NinjaInvocationOutputs(invocation).front();

            /* Find where the generated dependency info should go. */
            pbxsetting::Environment const &environment = targetEnvironment.environment();
            std::string temporaryDirectory = environment.resolve("TARGET_TEMP_DIR");
            dependencyInfoFile = temporaryDirectory + "/" + ".ninja-dependency-info-" + NinjaHash(output) + ".d";

            /* Build the dependency info rewriter arguments. */
            std::string dependencyInfoExecutable = NinjaDependencyInfoExecutable();
            std::vector<std::string> dependencyInfoArguments = {
                "--name", output,
                "--output", dependencyInfoFile,
            };

            /* Add the input for each dependency info. */
            for (pbxbuild::Tool::Invocation::DependencyInfo const &dependencyInfo : invocation.dependencyInfo()) {
                std::string formatName;
                if (!dependency::DependencyInfoFormats::Name(dependencyInfo.format(), &formatName)) {
                    return false;
                }

                dependencyInfoArguments.push_back(formatName + ":" + dependencyInfo.path());
            }

            /* Create the command for converting the dependency info. */
            dependencyInfoExec = Escape::Shell(dependencyInfoExecutable);
            for (std::string const &arg : dependencyInfoArguments) {
                dependencyInfoExec += " " + Escape::Shell(arg);
            }
        } else {
            // TODO(grp): Avoid the need for an empty dependency info command if not used.
            dependencyInfoExec = "true";
        }

        /*
         * Build up the bindings for the invocation.
         */
        std::vector<ninja::Binding> bindings = {
            { "description", ninja::Value::String(description) },
            { "dir", ninja::Value::String(Escape::Shell(invocation.workingDirectory())) },
            { "exec", ninja::Value::String(exec) },
        };
        if (!environment.empty()) {
            bindings.push_back({ "env", ninja::Value::String(environment) });
        }
        if (!dependencyInfoExec.empty()) {
            bindings.push_back({ "depexec", ninja::Value::String(dependencyInfoExec) });
        }
        if (!dependencyInfoFile.empty()) {
            bindings.push_back({ "depfile", ninja::Value::String(dependencyInfoFile) });
        }

        /*
         * Build up outputs as literal Ninja values.
         */
        std::vector<ninja::Value> outputs;
        for (std::string const &output : NinjaInvocationOutputs(invocation)) {
            outputs.push_back(ninja::Value::String(output));
        }

        /*
         * Build up inputs as literal Ninja values.
         */
        std::vector<ninja::Value> inputs;
        for (std::string const &input : invocation.inputs()) {
            inputs.push_back(ninja::Value::String(input));
        }

        /*
         * Build up input dependencies as literal Ninja values.
         */
        std::vector<ninja::Value> inputDependencies;
        for (std::string const &inputDependency : invocation.inputDependencies()) {
            inputDependencies.push_back(ninja::Value::String(inputDependency));
        }

        /*
         * Build up order dependencies as literal Ninja values.
         */
        std::vector<ninja::Value> orderDependencies;
        for (std::string const &orderDependency : invocation.orderDependencies()) {
            orderDependencies.push_back(ninja::Value::String(orderDependency));
        }

        /*
         * All invocations depend on the target containing them beginning.
         */
        orderDependencies.push_back(ninja::Value::String(targetBegin));

        /*
         * Add the rule to build this invocation.
         */
        writer.build(outputs, NinjaRuleName(), inputs, bindings, inputDependencies, orderDependencies);
    }

    /*
     * Serialize the Ninja file into the build root.
     */
    std::string path = TargetNinjaPath(target, targetEnvironment);
    if (!WriteNinja(filesystem, writer, path)) {
        fprintf(stderr, "error: unable to write target ninja: %s\n", path.c_str());
        return false;
    }

    return true;
}

std::unique_ptr<NinjaExecutor> NinjaExecutor::
Create(std::shared_ptr<xcformatter::Formatter> const &formatter, bool dryRun, bool generate)
{
    return std::unique_ptr<NinjaExecutor>(new NinjaExecutor(
        formatter,
        dryRun,
        generate
    ));
}
