/* SPDX-License-Identifier: MIT */

#ifndef _DRM_GPU_SCHEDULER_INTERNAL_H_
#define _DRM_GPU_SCHEDULER_INTERNAL_H_

#include <linux/ktime.h>
#include <linux/kref.h>
#include <linux/spinlock.h>

struct drm_sched_entity_stats {
	struct kref	kref;
	spinlock_t	lock;
	ktime_t		runtime;
};

/* Used to choose between FIFO and RR job-scheduling */
extern int drm_sched_policy;

#define DRM_SCHED_POLICY_RR    0
#define DRM_SCHED_POLICY_FIFO  1

bool drm_sched_can_queue(struct drm_gpu_scheduler *sched,
			 struct drm_sched_entity *entity);
void drm_sched_wakeup(struct drm_gpu_scheduler *sched);

void drm_sched_rq_init(struct drm_gpu_scheduler *sched,
		       struct drm_sched_rq *rq);
struct drm_sched_entity *
drm_sched_rq_select_entity(struct drm_gpu_scheduler *sched,
			   struct drm_sched_rq *rq);
struct drm_gpu_scheduler *
drm_sched_rq_add_entity(struct drm_sched_entity *entity, ktime_t ts);
void drm_sched_rq_remove_entity(struct drm_sched_rq *rq,
				struct drm_sched_entity *entity);
void drm_sched_rq_pop_entity(struct drm_sched_entity *entity);

void drm_sched_entity_select_rq(struct drm_sched_entity *entity);
struct drm_sched_job *drm_sched_entity_pop_job(struct drm_sched_entity *entity);

struct drm_sched_fence *drm_sched_fence_alloc(struct drm_sched_entity *s_entity,
					      void *owner);
void drm_sched_fence_init(struct drm_sched_fence *fence,
			  struct drm_sched_entity *entity);
void drm_sched_fence_free(struct drm_sched_fence *fence);

void drm_sched_fence_scheduled(struct drm_sched_fence *fence,
			       struct dma_fence *parent);
void drm_sched_fence_finished(struct drm_sched_fence *fence, int result);

/**
 * drm_sched_entity_queue_pop - Low level helper for popping queued jobs
 *
 * @entity: scheduler entity
 *
 * Low level helper for popping queued jobs.
 *
 * Returns: The job dequeued or NULL.
 */
static inline struct drm_sched_job *
drm_sched_entity_queue_pop(struct drm_sched_entity *entity)
{
	struct spsc_node *node;

	node = spsc_queue_pop(&entity->job_queue);
	if (!node)
		return NULL;

	return container_of(node, struct drm_sched_job, queue_node);
}

/**
 * drm_sched_entity_queue_peek - Low level helper for peeking at the job queue
 *
 * @entity: scheduler entity
 *
 * Low level helper for peeking at the job queue
 *
 * Returns: The job at the head of the queue or NULL.
 */
static inline struct drm_sched_job *
drm_sched_entity_queue_peek(struct drm_sched_entity *entity)
{
	struct spsc_node *node;

	node = spsc_queue_peek(&entity->job_queue);
	if (!node)
		return NULL;

	return container_of(node, struct drm_sched_job, queue_node);
}

/* Return true if entity could provide a job. */
static inline bool
drm_sched_entity_is_ready(struct drm_sched_entity *entity)
{
	if (!spsc_queue_count(&entity->job_queue))
		return false;

	if (READ_ONCE(entity->dependency))
		return false;

	return true;
}

void drm_sched_entity_stats_release(struct kref *kref);

static inline struct drm_sched_entity_stats *
drm_sched_entity_stats_get(struct drm_sched_entity_stats *stats)
{
	kref_get(&stats->kref);

	return stats;
}

static inline void
drm_sched_entity_stats_put(struct drm_sched_entity_stats *stats)
{
	kref_put(&stats->kref, drm_sched_entity_stats_release);
}

static inline void
drm_sched_entity_stats_job_add_gpu_time(struct drm_sched_job *job)
{
	struct drm_sched_entity_stats *stats = job->entity_stats;
	struct drm_sched_fence *s_fence = job->s_fence;
	ktime_t start, end;

	start = dma_fence_timestamp(&s_fence->scheduled);
	end = dma_fence_timestamp(&s_fence->finished);

	spin_lock(&stats->lock);
	stats->runtime = ktime_add(stats->runtime, ktime_sub(end, start));
	spin_unlock(&stats->lock);
}

#endif
