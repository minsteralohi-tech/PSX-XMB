"""
PSX-iTests intro exporter.

Run inside Blender (Scripting tab -> Open -> this file -> Run Script, or
Text Editor -> paste -> Run Script). Exports every mesh object in the
collection named EXPORT_COLLECTION (default "PS1_EXPORT") to a single plain
text file next to the .blend, which is then handed to the dashboard side of
this pipeline for conversion into C data tables.

WHY A TEXT DUMP INSTEAD OF FBX/glTF
------------------------------------
This deliberately does none of the PS1-specific math itself - no fixed-point
conversion, no angle-unit conversion, no int16 range checking. All of that
happens in a second pass on the receiving end, in plain Python with no bpy
dependency, where it can be tested and iterated on without touching Blender
at all. This script's only job is to get the raw scene data - vertices,
faces, per-frame world transforms - out of Blender reliably.

A generic format (FBX/glTF) was considered and rejected: parsing arbitrary
FBX/glTF correctly (skinning, coordinate conventions, material graphs) is a
much bigger and more error-prone job than reading a few plain-text lines,
and none of that generality is needed here.

MODELLING RULES THIS EXPECTS
-----------------------------
  - Every exported mesh must be QUADS or TRIS only (no n-gons). Blender's
    own "Select > By Trait > Faces by Sides" panel finds violations, or run
    Mesh > Face > Triangulate Faces if unsure.
  - Keep each rigid piece as its own object. Animate objects moving/rotating
    as a whole (the classic "pieces fly in and assemble" logo intro) rather
    than deforming a single mesh - this exporter samples each object's WORLD
    TRANSFORM per frame, not per-vertex deformation, which is what keeps the
    exported data small enough to fit in this project's RAM budget (tens of
    KB, not megabytes).
  - Colour comes from each face's assigned material's Viewport Display
    colour (the swatch in the Material Properties tab, not a texture) - flat
    per-face colour, matching this project's existing low-poly rendering
    (see src/main/gpu_cube.c). A face with no material exports as grey.
  - Complex non-rigid motion (organic deformation, cloth, etc.) is NOT
    supported by this exporter - see the guide text for why.
"""

import bpy
import os

# ---- configuration --------------------------------------------------------

EXPORT_COLLECTION = "PS1_EXPORT"   # only objects in this collection are exported
SAMPLE_RATE_HZ     = 20            # transform samples per second of animation
OUTPUT_FILENAME    = "intro_export.txt"

# ----------------------------------------------------------------------------


def find_export_objects():
	if EXPORT_COLLECTION not in bpy.data.collections:
		raise RuntimeError(
			f"No collection named '{EXPORT_COLLECTION}' found. Create it and "
			f"move every object you want exported into it, then run again."
		)

	objects = [
		obj for obj in bpy.data.collections[EXPORT_COLLECTION].all_objects
		if obj.type == 'MESH'
	]

	if not objects:
		raise RuntimeError(
			f"'{EXPORT_COLLECTION}' has no mesh objects in it."
		)

	return objects


def face_color(obj, poly):
	"""(r, g, b) in 0..255 from the polygon's assigned material, or grey."""
	if poly.material_index < len(obj.material_slots):
		slot = obj.material_slots[poly.material_index]
		if slot.material is not None:
			c = slot.material.diffuse_color   # RGBA, 0..1 linear
			return (
				round(max(0.0, min(1.0, c[0])) * 255),
				round(max(0.0, min(1.0, c[1])) * 255),
				round(max(0.0, min(1.0, c[2])) * 255),
			)

	return (128, 128, 128)


def write_mesh(f, obj):
	mesh = obj.data
	mesh.calc_loop_triangles()   # unused directly, but validates the mesh

	bad = [p.index for p in mesh.polygons if len(p.vertices) not in (3, 4)]
	if bad:
		raise RuntimeError(
			f"'{obj.name}': {len(bad)} face(s) are not tris/quads "
			f"(face indices: {bad[:10]}{'...' if len(bad) > 10 else ''}). "
			f"Triangulate before exporting."
		)

	f.write(f"MESH {obj.name} {len(mesh.vertices)} {len(mesh.polygons)}\n")

	# Local-space vertex positions. World placement is supplied separately,
	# per frame, by the ANIM block below - this is the rest-pose shape only.
	for v in mesh.vertices:
		f.write(f"V {v.co.x:.6f} {v.co.y:.6f} {v.co.z:.6f}\n")

	for poly in mesh.polygons:
		verts = list(poly.vertices)
		if len(verts) == 3:
			verts.append(verts[2])   # repeat last vertex: triangle as a degenerate quad
		r, g, b = face_color(obj, poly)
		f.write(f"F {verts[0]} {verts[1]} {verts[2]} {verts[3]} "
		        f"{r:02X}{g:02X}{b:02X}\n")


def write_anim(f, obj, scene):
	fps = scene.render.fps / scene.render.fps_base
	start, end = scene.frame_start, scene.frame_end
	duration_s = (end - start) / fps

	step = fps / SAMPLE_RATE_HZ
	sample_count = max(1, int(duration_s * SAMPLE_RATE_HZ) + 1)

	f.write(f"ANIM {obj.name} {sample_count} {SAMPLE_RATE_HZ}\n")

	original_frame = scene.frame_current

	for i in range(sample_count):
		frame = min(start + i * step, end)   # last sample can overshoot end by <1 step
		scene.frame_set(int(frame), subframe=frame - int(frame))

		# World transform, decomposed. Quaternion rather than reading
		# rotation_euler directly, so this is correct regardless of the
		# object's rotation mode or any parent/constraint driving it.
		translation, rotation, scale = obj.matrix_world.decompose()
		euler = rotation.to_euler('XYZ')

		f.write(
			f"T {translation.x:.6f} {translation.y:.6f} {translation.z:.6f} "
			f"{euler.x:.6f} {euler.y:.6f} {euler.z:.6f} "
			f"{scale.x:.6f} {scale.y:.6f} {scale.z:.6f}\n"
		)

	scene.frame_set(original_frame)


def main():
	scene = bpy.context.scene
	objects = find_export_objects()

	blend_dir = os.path.dirname(bpy.data.filepath) or os.path.expanduser("~")
	out_path = os.path.join(blend_dir, OUTPUT_FILENAME)

	with open(out_path, "w") as f:
		f.write(f"# PSX-iTests intro export\n")
		f.write(f"# scene fps: {scene.render.fps}/{scene.render.fps_base}\n")
		f.write(f"# frame range: {scene.frame_start}..{scene.frame_end}\n")
		f.write(f"OBJECTS {len(objects)}\n")

		for obj in objects:
			write_mesh(f, obj)
			write_anim(f, obj, scene)

	print(f"[PSX-iTests] wrote {out_path}")
	print(f"[PSX-iTests] {len(objects)} object(s) exported - send this file back.")


main()
