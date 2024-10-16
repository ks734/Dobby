/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2020 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "OOMCrashPlugin.h"
/**
 * Need to do this at the start of every plugin to make sure the correct
 * C methods are visible to allow PluginLauncher to find the plugin
 */
REGISTER_RDK_PLUGIN(OOMCrash);

/**
 * @brief Constructor - called when plugin is loaded by PluginLauncher
 *
 * Do not change the parameters for this constructor - must match C methods
 * created by REGISTER_RDK_PLUGIN macro
 *
 * Note plugin name is not case sensitive
 */
OOMCrash::OOMCrash(std::shared_ptr<rt_dobby_schema> &containerConfig,
                             const std::shared_ptr<DobbyRdkPluginUtils> &utils,
                             const std::string &rootfsPath)
    : mName("OOMCrash"),
      mContainerConfig(containerConfig),
      mRootfsPath(rootfsPath),
      mUtils(utils)
{
    AI_LOG_FN_ENTRY();

    AI_LOG_FN_EXIT();
}

/**
 * @brief Set the bit flags for which hooks we're going to use
 *
 * This plugin uses the postInstallation and postHalt hooks, so set those flags
 */
unsigned OOMCrash::hookHints() const
{
    return (
        IDobbyRdkPlugin::HintFlags::CreateRuntimeFlag |
	IDobbyRdkPlugin::HintFlags::PostHaltFlag);
}

/**
 *  * @brief Dobby Hook - run in host namespace *once* when container bundle is downloaded
 *   */
bool OOMCrash::CreateRuntime()
{
    if (!mContainerConfig)
    {
        AI_LOG_WARN("Container config is null");
        return false;
    }

    // get the container pid
    pid_t containerPid = mUtils->getContainerPid();
    if (!containerPid)
    {
        AI_LOG_ERROR_EXIT("couldn't find container pid");
        return false;
    }
    AI_LOG_INFO("###DBG : Container PID = %d", containerPid);
	
    const char *path = mContainerConfig->rdk_plugins->oomcrash->data->path;
    if (!mUtils->mkdirRecursive(mRootfsPath + path.c_str(), 0777) && errno != EEXIST)
    {
        AI_LOG_ERROR("failed to create directory '%s' (%d - %s)", (mRootfsPath + path).c_str(), errno, strerror(errno));
        return false;
    }
    AI_LOG_INFO("###DBG : path = %s", (mRootfsPath + path).c_str());

    char *command[] = {
        "nsenter", 
        "-t", containerPid, 
        "-m", 
        "mount", 
        "-t", "debugfs", 
        "debugfs", 
        path, 
        NULL
    };

    AI_LOG_INFO("###DBG : command = %s", command);
    pid_t pid_fork = fork();
    if (pid_fork < 0) {
        fprintf(stderr, "Error: fork failed (%d - %s)\n", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pid_fork == 0) {
        // Child process
        execvp(command[0], command);
        // If execvp returns, it must have failed
        LOG_ERR("failed exec '%s' (%d - %s)", command[0], errno, strerror(errno));
        return false;
    } else {
        // Parent process
        int status;
        waitpid(pid_fork, &status, 0);
        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            if (exit_status == 0) {
                AI_LOG_INFO("###DBG : Successfully executed nsenter");
            } else {
                AI_LOG_ERROR_EXIT("nsenter command failed, exit status: %d", exit_status);
            }
        } else {
            AI_LOG_ERROR_EXIT("Child process did not terminate normally");
        }
    }

//Chmod 777 
    if (chmod("/sys/kernel/debug/tracing/", 0777) == -1) {
        AI_LOG_ERROR("Error changing permissions");
    else
	AI_LOG_INFO("###DBG : Successfully changed permissions");
    return true;
}

/**
 * @brief Dobby Hook - Run in host namespace when container terminates
 */
bool OOMCrash::postHalt()
{
    if (!mContainerConfig)
    {
        AI_LOG_WARN("Container config is null");
        return false;
    }
    
    struct stat buffer;
    const char *targetpath = mContainerConfig->rdk_plugins->oomcrash->data->path;
    AI_LOG_INFO("###DBG : target path = %s", (mRootfsPath + targetpath).c_str());
    
    if (stat(mRootfsPath + targetPath.c_str(), &buffer) == 0)
    {
        if (umount(mRootfsPath + targetPath.c_str()) == 0)
            AI_LOG_INFO("###DBG : Successfully unmounted");
    }
	
    AI_LOG_FN_EXIT();
    return true;
}

// End hook methods

/**
 * @brief Should return the names of the plugins this plugin depends on.
 *
 * This can be used to determine the order in which the plugins should be
 * processed when running hooks.
 *
 * @return Names of the plugins this plugin depends on.
 */

std::vector<std::string> OOMCrash::getDependencies() const
{
    std::vector<std::string> dependencies;
    const rt_defs_plugins_oom_crash* pluginConfig = mContainerConfig->rdk_plugins->oomcrash;

    for (size_t i = 0; i < pluginConfig->depends_on_len; i++)
    {
        dependencies.push_back(pluginConfig->depends_on[i]);
    }

    return dependencies;
}
