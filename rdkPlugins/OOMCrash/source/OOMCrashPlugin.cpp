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

#include <map>

#define FIREBOLT_STATE "fireboltState"
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
        IDobbyRdkPlugin::HintFlags::PostInstallationFlag |
    IDobbyRdkPlugin::HintFlags::PostHaltFlag);
}

/**
 *  * @brief Dobby Hook - run in host namespace *once* when container bundle is downloaded
 *   */
bool OOMCrash::postInstallation()
{
    if (!mContainerConfig || !mContainerConfig->rdk_plugins || 
        !mContainerConfig->rdk_plugins->oomcrash || 
        !mContainerConfig->rdk_plugins->oomcrash->data)
    {
        AI_LOG_WARN("Container config or plugin data is null");
        return false;
    }

    const std::string path = mContainerConfig->rdk_plugins->oomcrash->data->path;
    if (path.empty())
    {
        AI_LOG_ERROR("OOMCrash path is empty");
        return false;
    }

    if (!mUtils->mkdirRecursive((mRootfsPath + path).c_str(), 0755) && errno != EEXIST)
    {
        AI_LOG_ERROR("failed to create directory '%s' (%d - %s)", (mRootfsPath + path).c_str(), errno, strerror(errno));
        return false;
    }

    if (!mUtils->mkdirRecursive(path.c_str(), 0755) && errno != EEXIST)
    {
        AI_LOG_ERROR("failed to create directory '%s' (%d - %s)", path.c_str(), errno, strerror(errno));
        return false;
    }

    if (!mUtils->addMount(path, path, "bind", {"bind", "ro", "nodev", "nosuid", "noexec"}))
    {
        AI_LOG_WARN("failed to add mount %s", path.c_str());
        return false;
    }

    AI_LOG_INFO("OOMCrash postInstallation hook is running for container with hostname %s", mUtils->getContainerId().c_str());
    return true;
}

/**
 * @brief Dobby Hook - Run in host namespace when container terminates
 */
bool OOMCrash::postHalt()
{
    if (!mContainerConfig || !mContainerConfig->rdk_plugins || 
        !mContainerConfig->rdk_plugins->oomcrash || 
        !mContainerConfig->rdk_plugins->oomcrash->data)
    {
        AI_LOG_WARN("Container config or plugin data is null");
        return false;
    }

    bool oomDetected = checkForOOM();

    if (oomDetected)
        createFileForOOM();

    // Remove the crashFile if container exits normally or if no OOM detected
    if (mUtils->exitStatus == 0 || !oomDetected)
    {
        std::string path = mContainerConfig->rdk_plugins->oomcrash->data->path;
        if (path.empty())
        {
            AI_LOG_ERROR("OOMCrash path is empty");
            return false;
        }

        std::string crashFile = path + "/oom_crashed_" + mUtils->getContainerId() + ".txt";
        if (remove(crashFile.c_str()) != 0)
        {
            if (errno != ENOENT)
            {
                perror("Failed to remove crash file");
                AI_LOG_WARN("Could not remove crash file: %s (%d - %s)", crashFile.c_str(), errno, strerror(errno));
            }
        }
        else
        {
            AI_LOG_INFO("%s file removed", crashFile.c_str());
        }
    }

    AI_LOG_INFO("OOMCrash postHalt hook is running for container with hostname %s", mUtils->getContainerId().c_str());
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

/**
 * @brief Read the oom_kill counter from the cgroup memory.oom_control file.
 *
 *  The memory.oom_control file contains multiple key-value lines, e.g.:
 *    oom_kill_disable 0
 *    under_oom        0
 *    oom_kill         1
 *
 *  @param[out]  val   Set to the value of the 'oom_kill' field on success.
 *
 * @return true on successfully reading and parsing the field.
 */

bool OOMCrash::readCgroup(unsigned long *val)
{
    std::string path = "/sys/fs/cgroup/memory/" + mUtils->getContainerId() + "/memory.oom_control";

    FILE *fp = fopen(path.c_str(), "r");
    if (!fp)
    {
        if (errno != ENOENT)
            AI_LOG_ERROR("failed to open '%s' (%d - %s)", path.c_str(), errno, strerror(errno));

        return false;
    }

    char* line = nullptr;
    size_t len = 0;
    ssize_t rd;
    bool found = false;

    while ((rd = getline(&line, &len, fp)) > 0)
    {
        unsigned long v;
        // sscanf won't match "oom_kill_disable" because the space in the
        // format requires whitespace where "_disable" has an underscore.
        if (sscanf(line, "oom_kill %lu", &v) == 1)
        {
            *val = v;
            found = true;
            break;
        }
    }

    if (line)
        free(line);
    fclose(fp);

    if (!found)
        AI_LOG_ERROR("oom_kill field not found in '%s'", path.c_str());

    return found;
}

/**
 * @brief Check for Out of Memory by reading cgroup file.
 *
 * @return true if OOM detected.
 */

bool OOMCrash::checkForOOM()
{
    unsigned long oomKill;
    if (!readCgroup(&oomKill))
    {
        AI_LOG_WARN("Failed to read oom_control for container '%s'", mUtils->getContainerId().c_str());
        return false;
    }

    if (oomKill == 0)
    {
        AI_LOG_INFO("No OOM kill detected in container '%s'", mUtils->getContainerId().c_str());
        return false;
    }

    // OOM kill confirmed - retrieve firebolt state from annotations
    std::map<std::string, std::string> annotations = mUtils->getAnnotations();
    auto it = annotations.find(FIREBOLT_STATE);
    if (it != annotations.end())
    {
        AI_LOG_WARN("OOM kill detected: container '%s' fireboltState '%s'",
                    mUtils->getContainerId().c_str(), it->second.c_str());
    }
    else
    {
        AI_LOG_WARN("OOM kill detected: container '%s' (firebolt state unknown)",
                    mUtils->getContainerId().c_str());
    }

    return true;
}

/**
 * @brief Create OOM crash file named oom_crashed_<container_name>.txt on the configured path.
 *
 */

void OOMCrash::createFileForOOM()
{
    std::string memoryExceedFile;
    std::string path = mContainerConfig->rdk_plugins->oomcrash->data->path;
    
    struct stat buffer;
    if (stat(path.c_str(), &buffer)==0)
    {
        memoryExceedFile = path + "/oom_crashed_" + mUtils->getContainerId() + ".txt";
        FILE *fp = fopen(memoryExceedFile.c_str(), "w+");
        if (!fp)
        {
            if (errno != ENOENT)
                AI_LOG_ERROR("failed to open '%s' (%d - %s)", path.c_str(), errno, strerror(errno));
        }
        else
        {
            AI_LOG_INFO("%s file created",memoryExceedFile.c_str());
            fclose(fp);
        }
    }
    else
    {
        if (errno == ENOENT)
            AI_LOG_ERROR("Path '%s' does not exist (%d - %s)", path.c_str(), errno, strerror(errno));
    }
}
