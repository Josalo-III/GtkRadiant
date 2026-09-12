textures/tests/stagewalk
{
	{
		map textures/tests/base
		blendfunc GL_DST_COLOR GL_SRC_COLOR
		tcMod scale 2 2
	}
	{
		clampmap $whiteimage
		blendFunc add
		animMap 4 textures/tests/a textures/tests/b
	}
}
