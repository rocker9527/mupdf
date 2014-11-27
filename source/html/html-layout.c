#include "mupdf/html.h"

enum { T, R, B, L };

static const char *default_css =
"html,address,blockquote,body,dd,div,dl,dt,h1,h2,h3,h4,h5,h6,ol,p,ul,center,hr,pre{display:block}"
"span{display:inline}"
"li{display:list-item}"
"head{display:none}"
"body{margin:1em}"
"h1{font-size:2em;margin:.67em 0}"
"h2{font-size:1.5em;margin:.75em 0}"
"h3{font-size:1.17em;margin:.83em 0}"
"h4,p,blockquote,ul,ol,dl,dir,menu{margin:1.12em 0}"
"h5{font-size:.83em;margin:1.5em 0}"
"h6{font-size:.75em;margin:1.67em 0}"
"h1,h2,h3,h4,h5,h6,b,strong{font-weight:bold}"
"blockquote{margin-left:40px;margin-right:40px}"
"i,cite,em,var,address{font-style:italic}"
"pre,tt,code,kbd,samp{font-family:monospace}"
"pre{white-space:pre}"
"big{font-size:1.17em}"
"small,sub,sup{font-size:.83em}"
"sub{vertical-align:sub}"
"sup{vertical-align:super}"
"s,strike,del{text-decoration:line-through}"
"hr{border-width:thin;border-color:black;border-style:solid;margin:.5em 0}"
"ol,ul,dir,menu,dd{margin-left:40px}"
"ol{list-style-type:decimal}"
"ol ul,ul ol,ul ul,ol ol{margin-top:0;margin-bottom:0}"
"u,ins{text-decoration:underline}"
"center{text-align:center}"
"svg{display:none}"
"a{color:blue}"
;

static int iswhite(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void fz_free_html_flow(fz_context *ctx, fz_html_flow *flow)
{
	while (flow)
	{
		fz_html_flow *next = flow->next;
		if (flow->type == FLOW_WORD)
			fz_free(ctx, flow->text);
		if (flow->type == FLOW_IMAGE)
			fz_drop_image(ctx, flow->image);
		fz_free(ctx, flow);
		flow = next;
	}
}

static fz_html_flow *add_flow(fz_context *ctx, fz_html_box *top, fz_css_style *style, int type)
{
	fz_html_flow *flow = fz_malloc_struct(ctx, fz_html_flow);
	flow->type = type;
	flow->style = style;
	*top->flow_tail = flow;
	top->flow_tail = &flow->next;
	return flow;
}

static void add_flow_space(fz_context *ctx, fz_html_box *top, fz_css_style *style)
{
	fz_html_flow *flow;

	/* delete space at the beginning of the line */
	if (!top->flow_head)
		return;

	flow = add_flow(ctx, top, style, FLOW_GLUE);
	flow->text = " ";
	flow->broken_text = "";
}

static void add_flow_word(fz_context *ctx, fz_html_box *top, fz_css_style *style, const char *a, const char *b)
{
	fz_html_flow *flow = add_flow(ctx, top, style, FLOW_WORD);
	flow->text = fz_malloc(ctx, b - a + 1);
	memcpy(flow->text, a, b - a);
	flow->text[b - a] = 0;
}

static void add_flow_image(fz_context *ctx, fz_html_box *top, fz_css_style *style, fz_image *img)
{
	fz_html_flow *flow = add_flow(ctx, top, style, FLOW_IMAGE);
	flow->image = fz_keep_image(ctx, img);
}

static void generate_text(fz_context *ctx, fz_html_box *box, const char *text)
{
	fz_html_box *flow = box;
	while (flow->type != BOX_FLOW)
		flow = flow->up;

	while (*text)
	{
		if (iswhite(*text))
		{
			++text;
			while (iswhite(*text))
				++text;
			add_flow_space(ctx, flow, &box->style);
		}
		if (*text)
		{
			const char *mark = text++;
			while (*text && !iswhite(*text))
				++text;
			add_flow_word(ctx, flow, &box->style, mark, text);
		}
	}
}

static void generate_image(fz_context *ctx, fz_archive *zip, const char *base_uri, fz_html_box *box, const char *src)
{
	fz_image *img;
	fz_buffer *buf;
	char path[2048];

	fz_html_box *flow = box;
	while (flow->type != BOX_FLOW)
		flow = flow->up;

	fz_strlcpy(path, base_uri, sizeof path);
	fz_strlcat(path, "/", sizeof path);
	fz_strlcat(path, src, sizeof path);
	fz_cleanname(path);

	fz_try(ctx)
	{
		buf = fz_read_archive_entry(ctx, zip, path);
		img = fz_new_image_from_buffer(ctx, buf);
		fz_drop_buffer(ctx, buf);

		add_flow_image(ctx, flow, &box->style, img);
	}
	fz_catch(ctx)
	{
		const char *alt = "[image]";
		fz_warn(ctx, "html: cannot add image src='%s'", src);
		add_flow_word(ctx, flow, &box->style, alt, alt + 7);
	}
}

static void init_box(fz_context *ctx, fz_html_box *box)
{
	box->type = BOX_BLOCK;
	box->x = box->y = 0;
	box->w = box->h = 0;

	box->up = NULL;
	box->last = NULL;
	box->down = NULL;
	box->next = NULL;

	box->flow_head = NULL;
	box->flow_tail = &box->flow_head;

	fz_default_css_style(ctx, &box->style);
}

void fz_free_html(fz_context *ctx, fz_html_box *box)
{
	while (box)
	{
		fz_html_box *next = box->next;
		fz_free_html_flow(ctx, box->flow_head);
		fz_free_html(ctx, box->down);
		fz_free(ctx, box);
		box = next;
	}
}

static fz_html_box *new_box(fz_context *ctx)
{
	fz_html_box *box = fz_malloc_struct(ctx, fz_html_box);
	init_box(ctx, box);
	return box;
}

static void insert_box(fz_context *ctx, fz_html_box *box, int type, fz_html_box *top)
{
	box->type = type;

	box->up = top;

	if (top)
	{
		if (!top->last)
		{
			top->down = top->last = box;
		}
		else
		{
			top->last->next = box;
			top->last = box;
		}
	}
}

static fz_html_box *insert_block_box(fz_context *ctx, fz_html_box *box, fz_html_box *top)
{
	if (top->type == BOX_BLOCK)
	{
		insert_box(ctx, box, BOX_BLOCK, top);
	}
	else if (top->type == BOX_FLOW)
	{
		while (top->type != BOX_BLOCK)
			top = top->up;
		insert_box(ctx, box, BOX_BLOCK, top);
	}
	else if (top->type == BOX_INLINE)
	{
		while (top->type != BOX_BLOCK)
			top = top->up;
		insert_box(ctx, box, BOX_BLOCK, top);
	}
	return top;
}

static fz_html_box *insert_break_box(fz_context *ctx, fz_html_box *box, fz_html_box *top)
{
	if (top->type == BOX_BLOCK)
	{
		insert_box(ctx, box, BOX_BREAK, top);
	}
	else if (top->type == BOX_FLOW)
	{
		while (top->type != BOX_BLOCK)
			top = top->up;
		insert_box(ctx, box, BOX_BREAK, top);
	}
	else if (top->type == BOX_INLINE)
	{
		while (top->type != BOX_BLOCK)
			top = top->up;
		insert_box(ctx, box, BOX_BREAK, top);
	}
	return top;
}

static void insert_inline_box(fz_context *ctx, fz_html_box *box, fz_html_box *top)
{
	if (top->type == BOX_BLOCK)
	{
		if (top->last && top->last->type == BOX_FLOW)
		{
			insert_box(ctx, box, BOX_INLINE, top->last);
		}
		else
		{
			fz_html_box *flow = new_box(ctx);
			flow->is_first_flow = !top->last;
			insert_box(ctx, flow, BOX_FLOW, top);
			insert_box(ctx, box, BOX_INLINE, flow);
		}
	}
	else if (top->type == BOX_FLOW)
	{
		insert_box(ctx, box, BOX_INLINE, top);
	}
	else if (top->type == BOX_INLINE)
	{
		insert_box(ctx, box, BOX_INLINE, top);
	}
}

static void generate_boxes(fz_context *ctx, fz_html_font_set *set, fz_archive *zip, const char *base_uri,
	fz_xml *node, fz_html_box *top, fz_css_rule *rule, fz_css_match *up_match)
{
	fz_css_match match;
	fz_html_box *box;
	const char *tag;
	int display;

	while (node)
	{
		match.up = up_match;
		match.count = 0;

		tag = fz_xml_tag(node);
		if (tag)
		{
			fz_match_css(ctx, &match, rule, node);

			display = fz_get_css_match_display(&match);

			if (!strcmp(tag, "br"))
			{
				box = new_box(ctx);
				fz_apply_css_style(ctx, set, &box->style, &match);
				top = insert_break_box(ctx, box, top);
			}

			else if (!strcmp(tag, "img"))
			{
				const char *src = fz_xml_att(node, "src");
				if (src)
				{
					box = new_box(ctx);
					fz_apply_css_style(ctx, set, &box->style, &match);
					insert_inline_box(ctx, box, top);
					generate_image(ctx, zip, base_uri, box, src);
				}
			}

			else if (display != DIS_NONE)
			{
				box = new_box(ctx);
				fz_apply_css_style(ctx, set, &box->style, &match);

				if (display == DIS_BLOCK)
				{
					top = insert_block_box(ctx, box, top);
				}
				else if (display == DIS_LIST_ITEM)
				{
					top = insert_block_box(ctx, box, top);
				}
				else if (display == DIS_INLINE)
				{
					insert_inline_box(ctx, box, top);
				}
				else
				{
					fz_warn(ctx, "unknown box display type");
					insert_box(ctx, box, BOX_BLOCK, top);
				}

				if (fz_xml_down(node))
					generate_boxes(ctx, set, zip, base_uri, fz_xml_down(node), box, rule, &match);

				// TODO: remove empty flow boxes
			}
		}
		else
		{
			if (top->type != BOX_INLINE)
			{
				box = new_box(ctx);
				insert_inline_box(ctx, box, top);
				box->style = top->style;
				generate_text(ctx, box, fz_xml_text(node));
			}
			else
			{
				generate_text(ctx, top, fz_xml_text(node));
			}
		}

		node = fz_xml_next(node);
	}
}

static void measure_image(fz_context *ctx, fz_html_flow *node, float w, float h)
{
	float xs = 1, ys = 1, s = 1;
	node->x = 0;
	node->y = 0;
	if (node->image->w > w)
		xs = w / node->image->w;
	if (node->image->h > h)
		ys = h / node->image->h;
	s = fz_min(xs, ys);
	node->w = node->image->w * s;
	node->h = node->image->h * s;
}

static void measure_word(fz_context *ctx, fz_html_flow *node, float em)
{
	const char *s;
	int c, g;
	float w;

	em = fz_from_css_number(node->style->font_size, em, em);
	node->x = 0;
	node->y = 0;
	node->h = fz_from_css_number_scale(node->style->line_height, em, em, em);

	w = 0;
	s = node->text;
	while (*s)
	{
		s += fz_chartorune(&c, s);
		g = fz_encode_character(ctx, node->style->font, c);
		w += fz_advance_glyph(ctx, node->style->font, g) * em;
	}
	node->w = w;
	node->em = em;
}

static float measure_line(fz_html_flow *node, fz_html_flow *end, float *baseline)
{
	float max_a = 0, max_d = 0, h = 0;
	while (node != end)
	{
		if (node->type == FLOW_IMAGE)
		{
			if (node->h > max_a)
				max_a = node->h;
		}
		else
		{
			float a = node->em * 0.8;
			float d = node->em * 0.2;
			if (a > max_a) max_a = a;
			if (d > max_d) max_d = d;
		}
		if (node->h > h) h = node->h;
		if (max_a + max_d > h) h = max_a + max_d;
		node = node->next;
	}
	*baseline = max_a + (h - max_a - max_d) / 2;
	return h;
}

static void layout_line(fz_context *ctx, float indent, float page_w, float line_w, int align, fz_html_flow *node, fz_html_flow *end, fz_html_box *box, float baseline)
{
	float x = box->x + indent;
	float y = box->y + box->h;
	float slop = page_w - line_w;
	float justify = 0;
	float va;
	int n = 0;

	if (align == TA_JUSTIFY)
	{
		fz_html_flow *it;
		for (it = node; it != end; it = it->next)
			if (it->type == FLOW_GLUE)
				++n;
		justify = slop / n;
	}
	else if (align == TA_RIGHT)
		x += slop;
	else if (align == TA_CENTER)
		x += slop / 2;

	while (node != end)
	{
		switch (node->style->vertical_align)
		{
		default:
		case VA_BASELINE:
			va = 0;
			break;
		case VA_SUB:
			va = node->em * 0.2f;
			break;
		case VA_SUPER:
			va = node->em * -0.3f;
			break;
		}
		node->x = x;
		if (node->type == FLOW_IMAGE)
			node->y = y + baseline - node->h;
		else
			node->y = y + baseline + va;
		x += node->w;
		if (node->type == FLOW_GLUE)
			x += justify;
		node = node->next;
	}
}

static fz_html_flow *find_next_glue(fz_html_flow *node, float *w)
{
	while (node && node->type == FLOW_GLUE)
	{
		*w += node->w;
		node = node->next;
	}
	while (node && node->type != FLOW_GLUE)
	{
		*w += node->w;
		node = node->next;
	}
	return node;
}

static fz_html_flow *find_next_word(fz_html_flow *node, float *w)
{
	while (node && node->type == FLOW_GLUE)
	{
		*w += node->w;
		node = node->next;
	}
	return node;
}

static void layout_flow(fz_context *ctx, fz_html_box *box, fz_html_box *top, float em, float page_h)
{
	fz_html_flow *node, *line_start, *word_start, *word_end, *line_end;
	float glue_w;
	float word_w;
	float line_w;
	float indent;
	float avail, line_h;
	float baseline;
	int align;

	em = fz_from_css_number(box->style.font_size, em, em);
	indent = box->is_first_flow ? fz_from_css_number(top->style.text_indent, em, top->w) : 0;
	align = top->style.text_align;

	box->x = top->x;
	box->y = top->y + top->h;
	box->w = top->w;
	box->h = 0;

	if (!box->flow_head)
		return;

	for (node = box->flow_head; node; node = node->next)
		if (node->type == FLOW_IMAGE)
			measure_image(ctx, node, top->w, page_h);
		else
			measure_word(ctx, node, em);

	line_start = find_next_word(box->flow_head, &glue_w);
	line_end = NULL;

	line_w = indent;
	word_w = 0;
	word_start = line_start;
	while (word_start)
	{
		word_end = find_next_glue(word_start, &word_w);
		if (line_w + word_w <= top->w)
		{
			line_w += word_w;
			glue_w = 0;
			line_end = word_end;
			word_start = find_next_word(word_end, &glue_w);
			word_w = glue_w;
		}
		else
		{
			avail = page_h - fmodf(box->y + box->h, page_h);
			line_h = measure_line(line_start, line_end, &baseline);
			if (line_h > avail)
				box->h += avail;
			layout_line(ctx, indent, top->w, line_w, align, line_start, line_end, box, baseline);
			box->h += line_h;
			word_start = find_next_word(line_end, &glue_w);
			line_start = word_start;
			line_end = NULL;
			indent = 0;
			line_w = 0;
			word_w = 0;
		}
	}

	/* don't justify the last line of a paragraph */
	if (align == TA_JUSTIFY)
		align = TA_LEFT;

	if (line_start)
	{
		avail = page_h - fmodf(box->y + box->h, page_h);
		line_h = measure_line(line_start, line_end, &baseline);
		if (line_h > avail)
			box->h += avail;
		layout_line(ctx, indent, top->w, line_w, align, line_start, line_end, box, baseline);
		box->h += line_h;
	}
}

static void layout_block(fz_context *ctx, fz_html_box *box, fz_html_box *top, float em, float top_collapse_margin, float page_h)
{
	fz_html_box *child;
	float box_collapse_margin;
	int prev_br;

	float *margin = box->margin;
	float *border = box->border;
	float *padding = box->padding;

	em = fz_from_css_number(box->style.font_size, em, em);

	margin[0] = fz_from_css_number(box->style.margin[0], em, top->w);
	margin[1] = fz_from_css_number(box->style.margin[1], em, top->w);
	margin[2] = fz_from_css_number(box->style.margin[2], em, top->w);
	margin[3] = fz_from_css_number(box->style.margin[3], em, top->w);

	padding[0] = fz_from_css_number(box->style.padding[0], em, top->w);
	padding[1] = fz_from_css_number(box->style.padding[1], em, top->w);
	padding[2] = fz_from_css_number(box->style.padding[2], em, top->w);
	padding[3] = fz_from_css_number(box->style.padding[3], em, top->w);

	if (box->style.border_style)
	{
		border[0] = fz_from_css_number(box->style.border_width[0], em, top->w);
		border[1] = fz_from_css_number(box->style.border_width[1], em, top->w);
		border[2] = fz_from_css_number(box->style.border_width[2], em, top->w);
		border[3] = fz_from_css_number(box->style.border_width[3], em, top->w);
	}
	else
		border[0] = border[1] = border[2] = border[3] = 0;

	if (padding[T] == 0 && border[T] == 0)
		box_collapse_margin = margin[T];
	else
		box_collapse_margin = 0;

	if (margin[T] > top_collapse_margin)
		margin[T] -= top_collapse_margin;
	else
		margin[T] = 0;

	box->x = top->x + margin[L] + border[L] + padding[L];
	box->y = top->y + top->h + margin[T] + border[T] + padding[T];
	box->w = top->w - (margin[L] + margin[R] + border[L] + border[R] + padding[L] + padding[R]);
	box->h = 0;

	prev_br = 0;
	for (child = box->down; child; child = child->next)
	{
		if (child->type == BOX_BLOCK)
		{
			layout_block(ctx, child, box, em, box_collapse_margin, page_h);
			box->h += child->h +
				child->padding[T] + child->padding[B] +
				child->border[T] + child->border[B] +
				child->margin[T] + child->margin[B];
			box_collapse_margin = child->margin[B];
			prev_br = 0;
		}
		else if (child->type == BOX_BREAK)
		{
			/* TODO: interaction with page breaks */
			if (prev_br)
				box->h += fz_from_css_number_scale(box->style.line_height, em, em, em);
			prev_br = 1;
		}
		else if (child->type == BOX_FLOW)
		{
			layout_flow(ctx, child, box, em, page_h);
			if (child->h > 0)
			{
				box->h += child->h;
				box_collapse_margin = 0;
				prev_br = 0;
			}
		}
	}

	if (padding[B] == 0 && border[B] == 0)
	{
		if (margin[B] > 0)
		{
			box->h -= box_collapse_margin;
			if (margin[B] < box_collapse_margin)
				margin[B] = box_collapse_margin;
		}
	}
}

static void draw_flow_box(fz_context *ctx, fz_html_box *box, float page_top, float page_bot, fz_device *dev, const fz_matrix *ctm)
{
	fz_html_flow *node;
	fz_text *text;
	fz_matrix trm;
	const char *s;
	float color[3];
	float x, y;
	int c, g;

	for (node = box->flow_head; node; node = node->next)
	{
		if (node->type == FLOW_IMAGE)
		{
			if (node->y > page_bot || node->y + node->h < page_top)
				continue;
		}
		else
		{
			if (node->y > page_bot || node->y < page_top)
				continue;
		}

		if (node->type == FLOW_WORD)
		{
			fz_scale(&trm, node->em, -node->em);
			text = fz_new_text(ctx, node->style->font, &trm, 0);

			x = node->x;
			y = node->y;
			s = node->text;
			while (*s)
			{
				s += fz_chartorune(&c, s);
				g = fz_encode_character(ctx, node->style->font, c);
				fz_add_text(ctx, text, g, c, x, y);
				x += fz_advance_glyph(ctx, node->style->font, g) * node->em;
			}

			color[0] = node->style->color.r / 255.0f;
			color[1] = node->style->color.g / 255.0f;
			color[2] = node->style->color.b / 255.0f;

			fz_fill_text(dev, text, ctm, fz_device_rgb(ctx), color, 1);

			fz_free_text(ctx, text);
		}
		else if (node->type == FLOW_IMAGE)
		{
			fz_matrix local_ctm = *ctm;
			fz_pre_translate(&local_ctm, node->x, node->y);
			fz_pre_scale(&local_ctm, node->w, node->h);
			fz_fill_image(dev, node->image, &local_ctm, 1);
		}
	}
}

static void draw_rect(fz_context *ctx, fz_device *dev, const fz_matrix *ctm, float *rgba, float x0, float y0, float x1, float y1)
{
	fz_path *path = fz_new_path(ctx);

	fz_moveto(ctx, path, x0, y0);
	fz_lineto(ctx, path, x1, y0);
	fz_lineto(ctx, path, x1, y1);
	fz_lineto(ctx, path, x0, y1);
	fz_closepath(ctx, path);

	fz_fill_path(dev, path, 0, ctm, fz_device_rgb(ctx), rgba, rgba[3]);

	fz_free_path(ctx, path);
}

static void draw_block_box(fz_context *ctx, fz_html_box *box, float page_top, float page_bot, fz_device *dev, const fz_matrix *ctm)
{
	float x0, y0, x1, y1;
	float color[4];

	// TODO: background fill
	// TODO: border stroke

	float *border = box->border;
	float *padding = box->padding;

	x0 = box->x - padding[L];
	y0 = box->y - padding[T];
	x1 = box->x + box->w + padding[R];
	y1 = box->y + box->h + padding[B];

	if (y0 > page_bot || y1 < page_top)
		return;

	if (box->style.background_color.a > 0)
	{
		color[0] = box->style.background_color.r / 255.0f;
		color[1] = box->style.background_color.g / 255.0f;
		color[2] = box->style.background_color.b / 255.0f;
		color[3] = box->style.background_color.a / 255.0f;
		draw_rect(ctx, dev, ctm, color, x0, y0, x1, y1);
	}

	if (box->style.border_color.a > 0)
	{
		color[0] = box->style.border_color.r / 255.0f;
		color[1] = box->style.border_color.g / 255.0f;
		color[2] = box->style.border_color.b / 255.0f;
		color[3] = box->style.border_color.a / 255.0f;
		if (border[T] > 0)
			draw_rect(ctx, dev, ctm, color, x0 - border[L], y0 - border[T], x1 + border[R], y0);
		if (border[B] > 0)
			draw_rect(ctx, dev, ctm, color, x0 - border[L], y1, x1 + border[R], y1 + border[B]);
		if (border[L] > 0)
			draw_rect(ctx, dev, ctm, color, x0 - border[L], y0 - border[T], x0, y1 + border[B]);
		if (border[R] > 0)
			draw_rect(ctx, dev, ctm, color, x1, y0 - border[T], x1 + border[R], y1 + border[B]);
	}

	for (box = box->down; box; box = box->next)
	{
		switch (box->type)
		{
		case BOX_BLOCK: draw_block_box(ctx, box, page_top, page_bot, dev, ctm); break;
		case BOX_FLOW: draw_flow_box(ctx, box, page_top, page_bot, dev, ctm); break;
		}
	}
}

void
fz_draw_html(fz_context *ctx, fz_html_box *box, float page_top, float page_bot, fz_device *dev, const fz_matrix *inctm)
{
	fz_matrix ctm = *inctm;
	fz_pre_translate(&ctm, 0, -page_top);
	draw_block_box(ctx, box, page_top, page_bot, dev, &ctm);
}

static char *concat_text(fz_context *ctx, fz_xml *root)
{
	fz_xml  *node;
	int i = 0, n = 1;
	char *s;
	for (node = fz_xml_down(root); node; node = fz_xml_next(node))
	{
		const char *text = fz_xml_text(node);
		n += text ? strlen(text) : 0;
	}
	s = fz_malloc(ctx, n);
	for (node = fz_xml_down(root); node; node = fz_xml_next(node))
	{
		const char *text = fz_xml_text(node);
		if (text)
		{
			n = strlen(text);
			memcpy(s+i, text, n);
			i += n;
		}
	}
	s[i] = 0;
	return s;
}

static fz_css_rule *
html_load_css(fz_context *ctx, fz_archive *zip, const char *base_uri, fz_css_rule *css, fz_xml *root)
{
	fz_xml *node;
	fz_buffer *buf;
	char path[2048];

	for (node = root; node; node = fz_xml_next(node))
	{
		const char *tag = fz_xml_tag(node);
		if (tag && !strcmp(tag, "link"))
		{
			char *rel = fz_xml_att(node, "rel");
			if (rel && !strcasecmp(rel, "stylesheet"))
			{
				char *type = fz_xml_att(node, "type");
				if ((type && !strcmp(type, "text/css")) || !type)
				{
					char *href = fz_xml_att(node, "href");
					if (href)
					{
						fz_strlcpy(path, base_uri, sizeof path);
						fz_strlcat(path, "/", sizeof path);
						fz_strlcat(path, href, sizeof path);
						fz_cleanname(path);

						buf = fz_read_archive_entry(ctx, zip, path);
						fz_write_buffer_byte(ctx, buf, 0);
						css = fz_parse_css(ctx, css, (char*)buf->data, path, 1);
						fz_drop_buffer(ctx, buf);
					}
				}
			}
		}
		if (tag && !strcmp(tag, "style"))
		{
			char *s = concat_text(ctx, node);
			css = fz_parse_css(ctx, css, s, "<style>", 1);
			fz_free(ctx, s);
		}
		if (fz_xml_down(node))
			css = html_load_css(ctx, zip, base_uri, css, fz_xml_down(node));
	}
	return css;
}

void
fz_layout_html(fz_context *ctx, fz_html_box *box, float w, float h, float em)
{
	fz_html_box page_box;

	printf("html: laying out text.\n");

	init_box(ctx, &page_box);
	page_box.w = w;
	page_box.h = 0;

	layout_block(ctx, box, &page_box, em, 0, h);

	printf("html: finished.\n");
}

fz_html_box *
fz_generate_html(fz_context *ctx, fz_html_font_set *set, fz_archive *zip, const char *base_uri, fz_buffer *buf)
{
	fz_xml *xml;
	fz_css_rule *css;
	fz_html_box *box;
	fz_css_match match;

	printf("html: parsing XHTML.\n");
	xml = fz_parse_xml(ctx, buf->data, buf->len, 1);

	printf("html: parsing style sheets.\n");
	css = fz_parse_css(ctx, NULL, default_css, "<default>", 1);
	css = html_load_css(ctx, zip, base_uri, css, xml);

	// print_rules(css);

	printf("html: applying styles and generating boxes.\n");
	box = new_box(ctx);

	match.up = NULL;
	match.count = 0;

	generate_boxes(ctx, set, zip, base_uri, xml, box, css, &match);

	fz_free_css(ctx, css);
	fz_free_xml(ctx, xml);

	return box;
}
