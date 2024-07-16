#pragma once

#include "utils/concurrency/synchronisation.h"
#include "utils/concurrency/task_priority.h"

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <queue>
#include <utility>

namespace dorado::utils::concurrency::detail {

using TaskType = std::function<void()>;

struct WaitingTask {
    WaitingTask() {}
    WaitingTask(TaskType task_, TaskPriority priority_)
            : task(std::move(task_)), priority(priority_) {}
    TaskType task{};
    TaskPriority priority{TaskPriority::normal};
};

/* 
 * Queue allowing tasks to be pushed and popped, also allows pop to be called
 * with a priority which will remove and return the next task with that priority
 * from the queue.
 */
class PriorityTaskQueue {
public:
    class TaskQueue {
    public:
        virtual ~TaskQueue() = default;
        virtual void push(TaskType task) = 0;
    };
    std::unique_ptr<TaskQueue> create_task_queue(TaskPriority priority);

    //void push(std::shared_ptr<WaitingTask> task);

    WaitingTask pop();
    WaitingTask pop(TaskPriority priority);

    std::size_t size() const;
    std::size_t size(TaskPriority priority) const;

    bool empty() const;
    bool empty(TaskPriority priority) const;

private:
    class ProducerQueue;

    using ProducerQueueList = std::list<ProducerQueue*>;
    ProducerQueueList m_producer_queue_list{};
    std::queue<ProducerQueueList::iterator> m_low_producer_queue{};
    std::queue<ProducerQueueList::iterator> m_high_producer_queue{};
    std::size_t m_num_normal_prio{};
    std::size_t m_num_high_prio{};

    using WaitingTaskList = std::list<std::shared_ptr<detail::WaitingTask>>;
    WaitingTaskList m_task_list{};
    std::queue<WaitingTaskList::iterator> m_low_queue{};
    std::queue<WaitingTaskList::iterator> m_high_queue{};

    void queue_producer_task(ProducerQueue* producer_queue);
};

}  // namespace dorado::utils::concurrency::detail