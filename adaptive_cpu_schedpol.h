/*
* Copyright (C) 2016  Politecnico di Milano
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef BBQUE_ADAPTIVE_CPU_SCHEDPOL_H_
#define BBQUE_ADAPTIVE_CPU_SCHEDPOL_H_

#include <cstdint>
#include <list>
#include <memory>

#include "bbque/configuration_manager.h"
#include "bbque/plugins/plugin.h"
#include "bbque/plugins/scheduler_policy.h"
#include "bbque/scheduler_manager.h"

#define SCHEDULER_POLICY_NAME "adaptive_cpu"

#define MODULE_NAMESPACE SCHEDULER_POLICY_NAMESPACE "." SCHEDULER_POLICY_NAME

using bbque::res::RViewToken_t;
using bbque::utils::MetricsCollector;
using bbque::utils::Timer;

// These are the parameters received by the PluginManager on create calls
struct PF_ObjectParams;

namespace bbque { namespace plugins {

struct AppInfo_t
{
    bbque::app::AppCPtr_t papp;
    bbque::app::AwmPtr_t pawm;
    uint64_t prev_quota;
    uint64_t prev_used;
    int64_t prev_delta;
    uint64_t next_quota;
};

class LoggerIF;

/**
* @class Adaptive_cpuSchedPol
*
* Adaptive_cpu scheduler policy registered as a dynamic C++ plugin.
*/
class Adaptive_cpuSchedPol: public SchedulerPolicyIF {

public:

    // :::::::::::::::::::::: Static plugin interface :::::::::::::::::::::::::

    /**
    * @brief Create the adaptive_cpu plugin
    */
    static void * Create(PF_ObjectParams *);

    /**
    * @brief Destroy the adaptive_cpu plugin 
    */
    static int32_t Destroy(void *);


    // :::::::::::::::::: Scheduler policy module interface :::::::::::::::::::

    /**
    * @brief Destructor
    */
    virtual ~Adaptive_cpuSchedPol();

    /**
    * @brief Return the name of the policy plugin
    */
    char const * Name();


    /**
    * @brief The member function called by the SchedulerManager to perform a
    * new scheduling / resource allocation
    */
    ExitCode_t Schedule(System & system, RViewToken_t & status_view);
    ExitCode_t AssignWorkingMode(bbque::app::AppCPtr_t papp);
    AppInfo_t InitializeAppInfo(bbque::app::AppCPtr_t papp);
    void ComputeQuota(AppInfo_t * ainfo);
    
private:

    /** Configuration manager instance */
    ConfigurationManager & cm;

    /** Resource accounter instance */
    ResourceAccounter & ra;

    /** System logger instance */
    std::unique_ptr<bu::Logger> logger;
    
    std::set<BBQUE_RID_TYPE> pe_ids;
    
    std::vector<uint32_t> sys_ids;
    
    uint64_t available_cpu;

    float kp;
    float ki;
    float kd;
    
    uint32_t nr_apps;

    /**
    * @brief Constructor
    *
    * Plugins objects could be build only by using the "create" method.
    * Usually the PluginManager acts as object
    */
    Adaptive_cpuSchedPol();

    /**
    * @brief Optional initialization member function
    */
    ExitCode_t _Init();

};

} // namespace plugins

} // namespace bbque

#endif // BBQUE_ADAPTIVE_CPU_SCHEDPOL_H_
