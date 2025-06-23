#include <linux/rbtree.h>

#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

#include "sched_internal.h"

static __always_inline bool
drm_sched_entity_compare_before(struct rb_node *a, const struct rb_node *b)
{
	struct drm_sched_entity *ea =
		rb_entry((a), struct drm_sched_entity, rb_tree_node);
	struct drm_sched_entity *eb =
		rb_entry((b), struct drm_sched_entity, rb_tree_node);

	return ktime_before(ea->oldest_job_waiting, eb->oldest_job_waiting);
}

static void drm_sched_rq_remove_tree_locked(struct drm_sched_entity *entity,
					    struct drm_sched_rq *rq)
{
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	if (!RB_EMPTY_NODE(&entity->rb_tree_node)) {
		rb_erase_cached(&entity->rb_tree_node, &rq->rb_tree_root);
		RB_CLEAR_NODE(&entity->rb_tree_node);
	}
}

static void drm_sched_rq_update_tree_locked(struct drm_sched_entity *entity,
					    struct drm_sched_rq *rq,
					    ktime_t ts)
{
	/*
	 * Both locks need to be grabbed, one to protect from entity->rq change
	 * for entity from within concurrent drm_sched_entity_select_rq and the
	 * other to update the rb tree structure.
	 */
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	drm_sched_rq_remove_tree_locked(entity, rq);

	entity->oldest_job_waiting = ts;

	rb_add_cached(&entity->rb_tree_node, &rq->rb_tree_root,
		      drm_sched_entity_compare_before);
}

/**
 * drm_sched_rq_init - initialize a given run queue struct
 *
 * @sched: scheduler instance to associate with this run queue
 * @rq: scheduler run queue
 *
 * Initializes a scheduler runqueue.
 */
void drm_sched_rq_init(struct drm_gpu_scheduler *sched,
		       struct drm_sched_rq *rq)
{
	spin_lock_init(&rq->lock);
	INIT_LIST_HEAD(&rq->entities);
	rq->rb_tree_root = RB_ROOT_CACHED;
	rq->sched = sched;
}

static ktime_t
drm_sched_rq_get_min_vruntime(struct drm_sched_rq *rq)
{
	struct drm_sched_entity *entity;
	struct rb_node *rb;

	lockdep_assert_held(&rq->lock);

	for (rb = rb_first_cached(&rq->rb_tree_root); rb; rb = rb_next(rb)) {
		entity = rb_entry(rb, typeof(*entity), rb_tree_node);

		return entity->stats->vruntime; /* Unlocked read */
	}

	return 0;
}

/**
 * drm_sched_rq_add_entity - add an entity
 *
 * @entity: scheduler entity
 * @ts: submission timestamp
 *
 * Adds a scheduler entity to the run queue.
 *
 * Returns a DRM scheduler pre-selected to handle this entity.
 */
struct drm_gpu_scheduler *
drm_sched_rq_add_entity(struct drm_sched_entity *entity)
{
	struct drm_gpu_scheduler *sched;
	struct drm_sched_rq *rq;
	ktime_t ts;

	/* Add the entity to the run queue */
	spin_lock(&entity->lock);
	if (entity->stopped) {
		spin_unlock(&entity->lock);

		DRM_ERROR("Trying to push to a killed entity\n");
		return NULL;
	}

	rq = entity->rq;
	spin_lock(&rq->lock);
	sched = rq->sched;

	if (list_empty(&entity->list)) {
		atomic_inc(sched->score);
		list_add_tail(&entity->list, &rq->entities);
	}

	ts = drm_sched_rq_get_min_vruntime(rq);
	ts = drm_sched_entity_restore_vruntime(entity, ts);
	drm_sched_rq_update_tree_locked(entity, rq, ts);

	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);

	return sched;
}

/**
 * drm_sched_rq_remove_entity - remove an entity
 *
 * @rq: scheduler run queue
 * @entity: scheduler entity
 *
 * Removes a scheduler entity from the run queue.
 */
void drm_sched_rq_remove_entity(struct drm_sched_rq *rq,
				struct drm_sched_entity *entity)
{
	lockdep_assert_held(&entity->lock);

	if (list_empty(&entity->list))
		return;

	spin_lock(&rq->lock);

	atomic_dec(rq->sched->score);
	list_del_init(&entity->list);

	drm_sched_rq_remove_tree_locked(entity, rq);

	spin_unlock(&rq->lock);
}

void drm_sched_rq_pop_entity(struct drm_sched_entity *entity)
{
	struct drm_sched_job *next_job;
	struct drm_sched_rq *rq;

	/*
	 * Update the entity's location in the min heap according to
	 * the timestamp of the next job, if any.
	 */
	spin_lock(&entity->lock);
	rq = entity->rq;
	spin_lock(&rq->lock);
	next_job = drm_sched_entity_queue_peek(entity);
	if (next_job) {
		ktime_t ts;

		ts = drm_sched_entity_get_job_ts(entity);
		drm_sched_rq_update_tree_locked(entity, rq, ts);
	} else {
		ktime_t min_vruntime;

		drm_sched_rq_remove_tree_locked(entity, rq);
		min_vruntime = drm_sched_rq_get_min_vruntime(rq);
		drm_sched_entity_save_vruntime(entity, min_vruntime);
	}
	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);
}

/**
 * drm_sched_rq_select_entity - Select an entity which provides a job to run
 *
 * @sched: the gpu scheduler
 * @rq: scheduler run queue to check.
 *
 * Find oldest waiting ready entity.
 *
 * Return an entity if one is found or NULL if no ready entity was found.
 */
struct drm_sched_entity *
drm_sched_rq_select_entity(struct drm_gpu_scheduler *sched,
			   struct drm_sched_rq *rq)
{
	struct rb_node *rb;

	spin_lock(&rq->lock);
	for (rb = rb_first_cached(&rq->rb_tree_root); rb; rb = rb_next(rb)) {
		struct drm_sched_entity *entity;

		entity = rb_entry(rb, struct drm_sched_entity, rb_tree_node);
		if (drm_sched_entity_is_ready(entity)) {
			reinit_completion(&entity->entity_idle);
			break;
		}
	}
	spin_unlock(&rq->lock);

	return rb ? rb_entry(rb, struct drm_sched_entity, rb_tree_node) : NULL;
}
