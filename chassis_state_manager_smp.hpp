#pragma once

#include "config.h"

#include <sdbusplus/bus.hpp>
#include <sdeventplus/clock.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/utility/timer.hpp>
#include <xyz/openbmc_project/State/Chassis/server.hpp>
#include <xyz/openbmc_project/State/PowerOnHours/server.hpp>

#include <map>
#include <memory>
#include <vector>

namespace phosphor::state::manager
{

using ChassisInherit = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::state::Chassis,
    sdbusplus::server::xyz::openbmc_project::state::PowerOnHours>;
namespace sdbusRule = sdbusplus::match_rules;

/** @class ChassisSMP
 *  @brief 多机箱 SMP 机箱状态管理聚合器。
 *  @details 聚合机箱实例 1 到 N 的状态信息，
 *           并将聚合结果呈现到机箱实例 0 上。
 *           该类会覆盖机箱实例 0 原本
 *           chassis_state_manager 的正常行为，
 *           使实例 0 仅用于聚合其他机箱实例的数据。
 */
class ChassisSMP : public ChassisInherit
{
  public:
    /** @brief 构造 Chassis SMP 状态聚合器
     *
     * @note 该构造函数向基类传入 'true'，以推迟 D-Bus 对象注册，
     *       直到我们能够从其他机箱实例确定初始状态后再注册。
     *
     * @param[in] bus        - D-Bus 总线对象
     * @param[in] objPath    - D-Bus 对象路径
     * @param[in] numChassis - 需要聚合的机箱实例数量（1 到 N）
     */
    ChassisSMP(sdbusplus::bus_t& bus, const sdbusplus::object_path& objPath,
               size_t numChassis);

    /** @brief 设置 RequestedPowerTransition 的值 */
    Transition requestedPowerTransition(Transition value) override;

    /** @brief 设置 CurrentPowerState 的值 */
    PowerState currentPowerState(PowerState value) override;

    /** @brief 开始监控各机箱实例 */
    void startMonitoring();

  private:
    /** @brief 处理机箱 0 目标的 systemd JobNew 信号
     *
     * 这确保当 systemd 目标被直接启动（而非通过 D-Bus API 启动）时，
     * 该状态切换仍会被转发到所有机箱实例，以维持 SMP 协调。
     * 同时监控机箱 0 的开机和关机目标。
     *
     * @param[in] msg - 包含作业信息的 D-Bus 消息
     */
    void sysStateChangeJobNew(sdbusplus::message_t& msg);

    /** @brief 处理机箱 0 目标的 systemd JobRemoved 信号
     *
     * 管理 poweronTargetComplete 标志，并触发 aggregatePowerState()
     * 以重新评估机箱 0 的电源状态。
     * 此外，在 obmc-chassis-poweron@0.target 完成后清除 chassis@0-on 文件。
     * 此操作必须在 JobRemoved 时执行（而不是在启动聚合阶段执行），
     * 以避免与 phosphor-reset-chassis-running@0 发生竞争，
     * 后者会在启动时并发写入该文件。
     *
     * @param[in] msg - 包含作业信息的 D-Bus 消息
     */
    void sysStateChangeJobRemoved(sdbusplus::message_t& msg);

    /** @brief 启动指定的 systemd 单元
     *
     * 该函数对给定的 systemd 单元调用 `StartUnit`。
     *
     * @param[in] sysdUnit    - systemd 单元名称
     */
    void startUnit(const std::string& sysdUnit);

    /** @brief 聚合所有机箱实例的电源状态
     *
     * 通过检查所有机箱实例来确定整体电源状态。
     * 如果任意一个机箱处于开机状态，则聚合状态为开机。
     */
    void aggregatePowerState();

    /** @brief 聚合所有机箱实例的电源状态健康状况
     *
     * 通过检查所有机箱实例来确定整体电源状态健康状况。
     * 采用最差情况原则（例如，如果任意一个机箱电源状态异常，则报告异常）。
     */
    void aggregatePowerStatus();

    /** @brief 处理被监控机箱实例的属性变更
     *
     * @param[in] msg - 包含属性变更的 D-Bus 消息
     * @param[in] chassisId - 发生变更的机箱 ID
     */
    void chassisPropertyChanged(sdbusplus::message_t& msg, size_t chassisId);

    /** @brief 向所有机箱实例发送电源切换请求
     *
     * @param[in] transition - 请求的电源切换动作
     */
    void requestTransitionOnAllChassis(Transition transition);

    /** @brief 检查某个机箱是否存在于库存中
     *
     * @param[in] chassisId - 要检查的机箱 ID
     * @return 如果机箱存在返回 true，否则返回 false
     */
    bool isChassisPresent(size_t chassisId);

    /** @brief 处理库存 Present 属性的变更
     *
     * @param[in] msg - 包含属性变更的 D-Bus 消息
     * @param[in] chassisId - 发生变更的机箱 ID
     */
    void inventoryPresentChanged(sdbusplus::message_t& msg, size_t chassisId);

    /** @brief 持久的 sdbusplus D-Bus 连接。 */
    sdbusplus::bus_t& bus;

    /** @brief 需要聚合的机箱实例数量（1 到 N）。 */
    const size_t numChassis;

    /** @brief 每个机箱实例的属性变更信号匹配对象。
     *  将机箱 ID 映射到其对应的匹配对象。 */
    std::map<size_t, std::unique_ptr<sdbusplus::match>> chassisMatches;

    /** @brief 库存 Present 属性变更的信号匹配对象。 */
    std::vector<std::unique_ptr<sdbusplus::match>> inventoryPresentMatches;

    /** @brief 用于监控机箱 0 目标的 systemd JobNew 信号匹配对象。 */
    sdbusplus::match systemdSignalJobNew;

    /** @brief 用于监控机箱 0 目标的 systemd JobRemoved 信号匹配对象。 */
    sdbusplus::match systemdSignalJobRemoved;

    /** @brief 来自各机箱实例的缓存电源状态。 */
    std::map<size_t, PowerState> chassisPowerStates;

    /** @brief 来自各机箱实例的缓存电源状态健康状况。 */
    std::map<size_t, PowerStatus> chassisPowerStatus;

    /** @brief 来自各机箱实例的缓存在线状态。 */
    std::map<size_t, bool> chassisPresentStatus;

    /** @brief 标志位，用于跟踪是否已因故障发起协调关机。
     *  防止在各机箱逐个关机时重复发出关机请求。 */
    bool coordinatedPowerOffInProgress = false;

    /** @brief 标志位，用于跟踪 obmc-chassis-poweron@0.target 的状态。 */
    bool poweronTargetComplete = false;
};

} // namespace phosphor::state::manager