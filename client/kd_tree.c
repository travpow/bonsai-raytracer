#include "kd_tree.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "measurements.h"
#include "util.h"

/**
 * Cost of traversing a KD-tree node relative to a mean cost of
 * intersecting an object (super magical constant).
 * Used for SAH when building a tree.
 * \todo Use some value based on measurements rather than this wild guess.
 */
#define TRAVERSAL_COST 0.5

/**
 * Bonus for the SAH heuristics for clipping away an empty space.
 */
#define CLIP_AWAY_EMPTY_SPACE_BONUS 0.2

/**
 * Maximal depth of the KD-tree.
 */
#define MAX_TREE_DEPTH 64

/**
 * State of the KD-tree building.
 */
struct kd_tree_build_state{
	unsigned nextNodeId;
	unsigned nextObjectId;
};

/**
 * A list of objects with count, pointer to the 
 * terminating NULL pointer and a bounding box.
 * Helper data structure for tree build.
 */
struct object_list{
	struct wrapped_object *head;
	struct wrapped_object **end;
	unsigned count;

	struct bounding_box box;
};

/**
 * Empty the object list.
 */
static void object_list_empty(struct object_list *l){
	l->head = NULL;
	l->end = &(l->head);
	l->count = 0;

	bounding_box_empty(&(l->box));
}

/**
 * Append an object to a last position of an object list.
 */
static void object_list_append(struct object_list *l, struct wrapped_object *o){
	o->next = NULL;

	*(l->end) = o;
	l->end = &(o->next);

	++(l->count);
}

/**
 * Build a object list from an array of objects.
 */
static void object_list_build(struct object_list *l, struct wrapped_object *objects){

	object_list_empty(l);

	struct wrapped_object *o = objects;
	while(o != NULL){
		struct wrapped_object *tmp = o->next;
		object_list_append(l, o);

		bounding_box_expand_box(&(l->box), &(o->o.boundingBox));
		o = tmp;
	}
}

/**
 * Initialize an empty tree.
 */
void kd_tree_init(struct kd_tree *tree){
	tree->nodes = NULL;
	tree->objects = NULL;
}

/**
 * Free all memory used by the nodes of a KD-tree.
 */
void kd_tree_destroy(struct kd_tree *tree){
	if(tree->nodes){
		free(tree->nodes);
		tree->nodes = NULL;
	}
	if(tree->objects){
		free(tree->objects);
		tree->objects = NULL;
	}
}

/**
 * An item on the stack for traversing a kd-tree.
 */
struct traversal_stack_item{
	unsigned nodeId;
	float lowerBound;
	float upperBound;
};

/**
 * Compute a first intersection of a scene and a ray with a distance
 * from the ray origin inside the closed interval
 * \f$ <lowerBound, upperBound> \f$, 
 * \return Distance to the intersection (in world coordinates),
 * This value is aways inside the closed interval \f$ <lowerBound, upperBound> \f$, 
 * or NAN if there was no intersection found.
 * \param tree KD-tree that should be traversed.
 * \param r Ray.
 * \param lowerBound Lower bound of the intersection distance.
 * \param upperBound Upper bound of the intersection distance.
 * \param[out] found Pointer to pointer to the found object.
 * If no intersection was found, then its value is left unchanged.
 * \pre lowerBound <= upperBound
 */
float kd_tree_ray_intersection(const struct kd_tree *tree,
	const struct ray *r, float lowerBound, float upperBound,
	struct object **result){

	unsigned nodeId = 0;

	struct traversal_stack_item stack[MAX_TREE_DEPTH];
	int stackIndex = -1;

	MEASUREMENTS_CLEAR_KD_TREE_STATS();

	while(true){
		MEASUREMENTS_TREE_TRAVERSAL();

		struct kd_tree_node *node = &((tree->nodes)[nodeId]);

		assert(lowerBound <= upperBound);

		if(node->flags & KD_TREE_NODE_LEAF_MASK){
			float distance = NAN;

			unsigned count = (node->flags & KD_TREE_NODE_COUNT_MASK) >>
				KD_TREE_NODE_COUNT_SHIFT;

			unsigned end = node->first + count;
			for(unsigned i = node->first; i < end; ++i){
				MEASUREMENTS_OBJECT_INTERSECTION();

				float tmp = object_ray_intersection(
					&((tree->objects)[i]),
					r, lowerBound, upperBound);

				if(!isnan(tmp)){
					distance = tmp;
					*result = &((tree->objects)[i]);
					upperBound = tmp;
				}
			}

			if(!isnan(distance)){
				return distance;
			}

			if(stackIndex == -1){
				return NAN;
			}

			nodeId = stack[stackIndex].nodeId;
			lowerBound = stack[stackIndex].lowerBound;
			upperBound = stack[stackIndex].upperBound;
			--stackIndex;

			continue;
		}

		// Find out where the ray intersects the splitting plane
		int axis = (node->flags & KD_TREE_NODE_AXIS_MASK) >>
			KD_TREE_NODE_AXIS_SHIFT;
		float splitDistance =
			(node->coord - r->origin.f[axis]) * r->invDirection.f[axis];

		// Calculating the order of traversal:
		// Four possibilities:
		// if ray direction > 0
		//   if node->frontMoreProbable
		//     first = back = node->lessProbableIndex
		//     second = front = nodeId + 1
		//   else
		//     first = back = nodeId + 1
		//     second = front = node->lessProbableIndex
		// else
		//   if node->frontMoreProbable
		//     first = front = nodeId + 1
		//     second = back = node->lessProbableIndex
		//   else
		//     first = front = node->lessProbableIndex
		//     second = back = nodeId + 1
		unsigned first;
		unsigned second;
		bool frontMoreProbable =
			(node->flags & KD_TREE_NODE_FRONT_MORE_PROBABLE_MASK) != 0;
		bool positiveDirection = (r->direction.f[axis] > 0);

		if(positiveDirection != frontMoreProbable){
			first = nodeId + 1;
			second = (node->flags & KD_TREE_NODE_LESS_PROBABLE_INDEX_MASK) >>
				KD_TREE_NODE_LESS_PROBABLE_INDEX_SHIFT;
		}else{
			first = (node->flags & KD_TREE_NODE_LESS_PROBABLE_INDEX_MASK) >>
				KD_TREE_NODE_LESS_PROBABLE_INDEX_SHIFT;
			second = nodeId + 1;
		}

		if(upperBound <= splitDistance){
			nodeId = first;
		}else if(lowerBound >= splitDistance){
			nodeId = second;
		}else{
			++stackIndex;
			stack[stackIndex].nodeId = second;
			stack[stackIndex].lowerBound = splitDistance;
			stack[stackIndex].upperBound = upperBound;

			nodeId = first;
			upperBound = splitDistance;
		}
	}
}

/**
 * Calculate the cost of not splitting the list of objects
 * (keeping the current node as a leaf).
 * Since the intersection cost is 1, this function becomes a little too ... simple.
 */
static inline float not_split_cost(unsigned count){
	return count;
}

/**
 * Split the given list on a plane.
 * Outputs two lists of objects that are in front of the plane,
 * and behind the plane. Objects that have a nonempty intersection with
 * the splitting plane are copied and placed into both lists.
 */
static void split_at(struct object_list *objs, int axis, float position,
	struct object_list *front, struct object_list *back){

	object_list_empty(front);
	object_list_empty(back);

	front->box = objs->box;
	back->box = objs->box;

	front->box.p[0].f[axis] = position;
	back->box.p[1].f[axis] = position;

	struct wrapped_object *o = objs->head;
	while(o != NULL){
		struct wrapped_object *tmp = o->next;

		if(o->o.boundingBox.p[0].f[axis] >= position){
			object_list_append(front, o);
		}else if(o->o.boundingBox.p[1].f[axis] <= position){
			object_list_append(back, o);
		}else{
			object_list_append(front, o);

			struct wrapped_object *o2 = checked_malloc(sizeof(*o2));
			*o2 = *o;
			object_list_append(back, o2);
		}

		o = tmp;
	}

	object_list_empty(objs);
}

/**
 * Calculate the SAH cost of a split.
 *
 * \note Uses the TRAVERSAL_COST constant.
 */
static float sah_split_cost(struct object_list *objs, int axis, float position){
	unsigned frontCount = 0;
	struct bounding_box frontBox = objs->box;
	frontBox.p[0].f[axis] = position;

	unsigned backCount = 0;
	struct bounding_box backBox = objs->box;
	backBox.p[1].f[axis] = position;

	for(struct wrapped_object *o = objs->head; o != NULL; o = o->next){
		if(o->o.boundingBox.p[0].f[axis] >= position){
			++frontCount;
		}else if(o->o.boundingBox.p[1].f[axis] <= position){
			++backCount;
		}else{
			// intersects the splitting plane.
			++frontCount;
			++backCount;
		}
	}

	float wholeArea = bounding_box_area(&(objs->box));

	// probabilities of hitting 
	float pFront = bounding_box_area(&(frontBox)) / wholeArea;
	float pBack = bounding_box_area(&(backBox)) / wholeArea;

	float cost = TRAVERSAL_COST + pFront * frontCount + pBack * backCount;

	if(frontCount == 0 || backCount == 0){
		cost *= 1 - CLIP_AWAY_EMPTY_SPACE_BONUS;
	}

	return cost;
}

/**
 * Recursively build a KD-tree using surface area heuristics.
 * The result of this function is in the depth first layout.
 * This function deallocates all nodes of the list in objs,
 * Realocates tree->nodes to be large enough to hold all nodes and reallocates
 * tree->objects to store all objects including the copies needed for objects
 * on both sides of the splitting plane.
 * \note Index of the node being built is in #state.
 * \param tree Tree that contains the current node.
 * \param objs List of objects to put into the tree.
 * \param depth How deep is the current node in the tree.
 * \param state Indices available for extending the tree.
 * \pre tree->nodes == NULL or points to allocated memory.
 * \pre tree->objects points to allocated memory, large enough to hold all objects.
 */
static void kd_tree_node_build(struct kd_tree *tree, struct object_list *objs,
	unsigned depth, struct kd_tree_build_state *state){

	float bestCost = not_split_cost(objs->count);
	bool bestIsNoSplit = true;
	int bestAxis = 0;
	float bestCoord = 0;

	unsigned nodeId = state->nextNodeId;
	++(state->nextNodeId);
	
	if(depth < MAX_TREE_DEPTH){
		// For every object try using all six bounding box faces 
		// as a kd-tree splitting planes.
		for(struct wrapped_object *o = objs->head; o != NULL; o = o->next){
			for(int axis = 0; axis < 3; ++axis){
				for(int i = 0; i < 2; ++i){
					float coord = o->o.boundingBox.p[i].f[axis];

					if(coord < objs->box.p[0].f[axis] ||
						coord > objs->box.p[1].f[axis]){
						// if the splitting plane would be
						// outside this node's accessible
						// box, then there's no point
						// in using it.
						continue;
					}
						

					float cost = sah_split_cost(objs, axis, coord);

					if(cost < bestCost){
						bestCost = cost;
						bestIsNoSplit = false;
						bestAxis = axis;
						bestCoord = coord;
					}
				}
			}
		}
	}

	tree->nodes = checked_realloc_x(
		tree->nodes,
		sizeof(struct kd_tree_node) * (nodeId + 1),
		sizeof(struct kd_tree_node) * nodeId);


	struct kd_tree_node *node = &(tree->nodes[nodeId]);

	unsigned flags = 0;
	
	if(bestIsNoSplit){
		flags |= KD_TREE_NODE_LEAF_MASK;
	}

	if(bestIsNoSplit){
		/* store the objects and free the wrapper list nodes */

		node->first = state->nextObjectId;

		flags |= (objs->count << KD_TREE_NODE_COUNT_SHIFT) &
			KD_TREE_NODE_COUNT_MASK;

		/* allocate space for the newly added objects */
		tree->objects = checked_realloc_x(tree->objects,
			(state->nextObjectId + objs->count) * sizeof(struct object),
			state->nextObjectId * sizeof(struct object));

		int i = state->nextObjectId;
		struct wrapped_object *o = objs->head;
		while(o != NULL){
			tree->objects[i] = o->o;

			struct wrapped_object *tmp = o->next;
			free(o);
			o = tmp;

			++i;
		}

		state->nextObjectId += objs->count;
	}else{
		struct object_list front, back;
		
		split_at(objs, bestAxis, bestCoord, &front, &back);

		bool frontMoreProbable = 
			bounding_box_area(&(front.box)) >
			bounding_box_area(&(back.box));

		unsigned lessProbableIndex;

		node->coord = bestCoord;

		if(frontMoreProbable){
			kd_tree_node_build(tree, &front, depth + 1, state);
			lessProbableIndex = state->nextNodeId;
			kd_tree_node_build(tree, &back, depth + 1, state);
		}else{
			kd_tree_node_build(tree, &back, depth + 1, state);
			lessProbableIndex = state->nextNodeId;
			kd_tree_node_build(tree, &front, depth + 1, state);
		}

		if(frontMoreProbable){
			flags |= KD_TREE_NODE_FRONT_MORE_PROBABLE_MASK;
		}

		flags |= (lessProbableIndex << KD_TREE_NODE_LESS_PROBABLE_INDEX_SHIFT) &
			KD_TREE_NODE_LESS_PROBABLE_INDEX_MASK;

		flags |= (bestAxis << KD_TREE_NODE_AXIS_SHIFT) &
			KD_TREE_NODE_AXIS_MASK;
	}

	tree->nodes[nodeId].flags = flags;
}

/**
 * Build a KD-tree from objects in linked list #objs.
 */
void kd_tree_build(struct kd_tree *tree, struct wrapped_object *objs){
	printf("Building the KD-tree...\n");
	printf("Node size = %u\n", sizeof(struct kd_tree_node));

	struct object_list list;
	object_list_build(&list, objs);

	tree->nodes = NULL;
	tree->objects = NULL;
	
	struct kd_tree_build_state state;
	state.nextNodeId = 0;
	state.nextObjectId = 0;
	kd_tree_node_build(tree, &list, 0, &state);

	printf("Done.\n");
	printf("(Node count = %d, object count (including copies) = %d)\n",
		state.nextNodeId, state.nextObjectId);
}
