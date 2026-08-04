#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/// 日程模块用例接口，仅定义能力边界，不包含具体存储和业务实现。
class ScheduleService {
   public:
    /** @brief Releases the service through its interface type. */
    virtual ~ScheduleService() = default;

    /**
     * @brief Creates a schedule.
     * @param command Data for the new schedule.
     * @return Creation result, including conflicts when present.
     */
    virtual CreateScheduleResult create_schedule(const CreateScheduleCommand& command) = 0;

    /**
     * @brief Cancels a schedule without automatically deleting its reminder.
     * @param command Schedule to cancel.
     * @return Deletion result.
     */
    virtual DeleteScheduleResult delete_schedule(const DeleteScheduleCommand& command) = 0;

    /**
     * @brief Updates only the supplied fields of a schedule.
     * @param command Schedule changes to apply.
     * @return Update result, including conflicts when present.
     */
    virtual UpdateScheduleResult update_schedule(const UpdateScheduleCommand& command) = 0;

    /**
     * @brief Queries schedules using filters and pagination.
     * @param command Query filters and page bounds.
     * @return Matching schedules and total count.
     */
    virtual QueryScheduleResult query_schedule(const QueryScheduleCommand& command) const = 0;

    /**
     * @brief Records a create, update, or delete operation.
     * @param command Operation details to persist.
     * @return Recorded operation result.
     */
    virtual RecordScheduleOperationResult record_schedule_operation(const RecordScheduleOperationCommand& command) = 0;

    /**
     * @brief Queries the ten most recent undoable operations.
     * @return Recent undoable operations.
     */
    virtual QueryRecentScheduleOperationResult query_recent_schedule_operation() const = 0;

    /**
     * @brief Undoes a selected operation when it is within the 15-minute window.
     * @param command Operation to undo.
     * @return Undo result and restored data when successful.
     */
    virtual UndoScheduleOperationResult undo_schedule_operation(const UndoScheduleOperationCommand& command) = 0;
};

}  // namespace voicelife::schedule
