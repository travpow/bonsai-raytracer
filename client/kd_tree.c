#include "kd_tree.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "util.h"

/**
 * Cost of traversing a KD-tree node relative to a mean cost of
 * intersecting an object (super magical constant).
 * Used for SAH when building a tree.
 * \todo Use some value based on measurements rather than this wild guess.
 */
#define TRAVERSAL_COST 0.5

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
 * Free all memory used by the nodes of a KD-tree.
 */
void kd_tree_destroy(struct kd_tree *tree){
	free(tree->nodes);
	free(tree->objects);
}

/**
 * Compute a first intersection of a scene and a ray with a distance
 * from the ray origin inside the closed interval
 * \f$ <lowerBound, upperBound> \f$, 
 * \return Distance to the intersection (in world coordinates),
 * This value is aways inside the closed interval \f$ <lowerBound, upperBound> \f$, 
 * or NAN if there was no intersection found.
 * \param nodes Array of KD-tree nodes.
 * \param r Ray.
 * \param lowerBound Lower bound of the intersection distance.
 * \param upperBound Upper bound of the intersection distance.
 * \param[out] found Pointer to pointer to the found object.
 * If no intersection was found, then its value is left unchanged.
 * \pre lowerBound <= upperBound
 */
float kd_tree_node_ray_intersection(const struct kd_tree_node *nodes, unsigned node,
	struct object *objects,
	const struct ray *r, float lowerBound, float upperBound,
	struct object **result){

	assert(lowerBound <= upperBound);

	if(nodes[node].leaf){
		float distance = NAN;

		unsigned end = nodes[node].first + nodes[node].count;
		for(unsigned i = nodes[node].first; i < end; ++i){
			float tmp = object_ray_intersection(&(objects[i]), r, lowerBound, upperBound);

			if(!isnan(tmp)){
				distance = tmp;
				*result = &(objects[i]);
				upperBound = tmp;
			}
		}

		return distance;
	}

	// Find out where the ray intersects the splitting plane
	int axis = nodes[node].axis;
	float splitDistance =
		(nodes[node].coord - r->origin.p[axis]) * r->invDirection.p[axis];

	// Calculating the order of traversal:
	// Four possibilities:
	// if ray direction > 0
	//   if nodes[node].frontMoreProbable
	//     first = back = nodes[node].lessProbableIndex
	//     second = front = node + 1
	//   else
	//     first = back = node + 1
	//     second = front = nodes[node].lessProbableIndex
	// else
	//   if nodes[node].frontMoreProbable
	//     first = front = node + 1
	//     second = back = nodes[node].lessProbableIndex
	//   else
	//     first = front = nodes[node].lessProbableIndex
	//     second = back = node + 1
	unsigned first;
	unsigned second;
	if((r->direction.p[axis] > 0) ^ (nodes[node].frontMoreProbable)){
		first = node + 1;
		second = nodes[node].lessProbableIndex;
	}else{
		first = nodes[node].lessProbableIndex;
		second = node + 1;
	}

	if(upperBound <= splitDistance){
		return kd_tree_node_ray_intersection(nodes, first, objects, r,
			lowerBound, upperBound, result);
	}else if(lowerBound >= splitDistance){
		return kd_tree_node_ray_intersection(nodes, second, objects, r,
			lowerBound, upperBound, result);
	}else{
		float tmp;
		tmp = kd_tree_node_ray_intersection(nodes, first, objects, r,
			lowerBound, splitDistance, result);
		if(!isnan(tmp)){
			return tmp;
		}else{
			return kd_tree_node_ray_intersection(nodes, second, objects, r,
				splitDistance, upperBound, result);
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

	front->box.p[0].p[axis] = position;
	back->box.p[1].p[axis] = position;

	struct wrapped_object *o = objs->head;
	while(o != NULL){
		struct wrapped_object *tmp = o->next;

		if(o->o.boundingBox.p[0].p[axis] >= position){
			object_list_append(front, o);
		}else if(o->o.boundingBox.p[1].p[axis] <= position){
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
 *
 * \todo Try giving 20% bonus for clipping away empty space?
 */
static float sah_split_cost(struct object_list *objs, int axis, float position){
	unsigned frontCount = 0;
	struct bounding_box frontBox = objs->box;
	frontBox.p[0].p[axis] = position;

	unsigned backCount = 0;
	struct bounding_box backBox = objs->box;
	backBox.p[1].p[axis] = position;

	for(struct wrapped_object *o = objs->head; o != NULL; o = o->next){
		if(o->o.boundingBox.p[0].p[axis] >= position){
			++frontCount;
		}else if(o->o.boundingBox.p[1].p[axis] <= position){
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
			for(int axis = 0; axis < DIMENSIONS; ++axis){
				for(int i = 0; i < 2; ++i){
					float coord = o->o.boundingBox.p[i].p[axis];

					if(coord < objs->box.p[0].p[axis] ||
						coord > objs->box.p[1].p[axis]){
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

	tree->nodes = checked_realloc(tree->nodes, sizeof(struct kd_tree_node) * (nodeId + 1));

	struct kd_tree_node *node = &(tree->nodes[nodeId]);

	node->leaf = bestIsNoSplit;

	if(bestIsNoSplit){
		/* store the objects and free the wrapper list nodes */

		node->count = objs->count;
		node->first = state->nextObjectId;


		/* allocate space for the newly added objects */
		tree->objects = checked_realloc(tree->objects,
			(state->nextObjectId + objs->count) *
			sizeof(struct object));

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

		if(frontMoreProbable){
			kd_tree_node_build(tree, &front, depth + 1, state);
			lessProbableIndex = state->nextNodeId;
			kd_tree_node_build(tree, &back, depth + 1, state);
		}else{
			kd_tree_node_build(tree, &back, depth + 1, state);
			lessProbableIndex = state->nextNodeId;
			kd_tree_node_build(tree, &front, depth + 1, state);
		}

		// we can't use the node pointer because kd_tree_node_build reallocs our node array
		tree->nodes[nodeId].frontMoreProbable = frontMoreProbable;
		tree->nodes[nodeId].lessProbableIndex = lessProbableIndex;
		tree->nodes[nodeId].axis = bestAxis;
		tree->nodes[nodeId].coord = bestCoord;
	}
}

/**
 * Build a KD-tree from objects in linked list #objs.
 */
void kd_tree_build(struct kd_tree *tree, struct wrapped_object *objs){
	printf("Building the KD-tree...\n");

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
