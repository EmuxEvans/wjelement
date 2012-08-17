/*
    This file is part of WJElement.

    WJElement is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    WJElement is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with WJElement.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "element.h"

void WJEChanged(WJElement element)
{
	for (; element; element = element->parent) {
		element->changes++;
	}
}

_WJElement * _WJENew(_WJElement *parent, char *name, size_t len, const char *file, int line)
{
	_WJElement	*result;
	WJElement	prev;

	if (parent) {
		_WJEChanged((WJElement) parent);

		if (WJR_TYPE_ARRAY == parent->pub.type) {
			/* Children of an array can not have a name */
			name	= NULL;
			len		= 0;
		} else if (WJR_TYPE_OBJECT != parent->pub.type || !name || !*name) {
			/*
				Only arrays and objects can contain children, and elements in an
				object MUST be named.
			*/
			return(NULL);
		}
	}

	if ((result = MemMalloc(sizeof(_WJElement) + len + 1))) {
		memset(result, 0, sizeof(_WJElement));

		MemUpdateOwner(result, file, line);

		if (name) {
			strncpy(result->_name, name, len);
			result->pub.name = result->_name;
		}
		result->_name[len] = '\0';

		if (parent) {
			result->pub.parent = (WJElement) parent;

			if (!parent->pub.child) {
				parent->pub.child = (WJElement) result;
			} else {
				/* Find the last child so it can be added at the end */
				for (prev = parent->pub.child; prev && prev->next; prev = prev->next);

				prev->next = (WJElement) result;
				result->pub.prev = prev;
			}

			parent->pub.count++;
		}

		result->pub.type = WJR_TYPE_OBJECT;
	}

	return(result);
}

_WJElement * _WJEReset(_WJElement *e, WJRType type)
{
	if (!e) return(NULL);

	while (e->pub.child) {
		WJECloseDocument(e->pub.child);
	}

	if (WJR_TYPE_STRING == e->pub.type && e->value.string) {
		MemRelease(&(e->value.string));
	}
	e->value.string	= NULL;
	e->pub.type		= type;

	return(e);
}

EXPORT XplBool _WJEDettach(WJElement document, const char *file, const int line)
{
	if (!document) {
		return(FALSE);
	}

	MemUpdateOwner(document, file, line);

	/* Remove references to the document */
	if (document->parent) {
		WJEChanged(document->parent);

		if (document->parent->child == document) {
			document->parent->child = document->next;
		}
		document->parent->count--;
		document->parent = NULL;
	}

	if (document->prev) {
		document->prev->next = document->next;
		document->prev = NULL;
	}

	if (document->next) {
		document->next->prev = document->prev;
		document->next = NULL;
	}

	return(TRUE);
}

EXPORT XplBool WJEAttach(WJElement container, WJElement document)
{
	WJElement	prev;

	if (!document || !container) {
		return(FALSE);
	}

	if (document->name) {
		while ((prev = WJEChild(container, document->name, WJE_GET))) {
			WJECloseDocument(prev);
		}
	}

	WJEDettach(document);

	/* Insert it into the new container */
	document->parent = container;
	if (!container->child) {
		container->child = document;
	} else {
		/* Find the last child so it can be added at the end */
		for (prev = container->child; prev && prev->next; prev = prev->next);

		prev->next = document;
		document->prev = prev;
	}
	container->count++;
	WJEChanged(container);

	return(TRUE);
}

EXPORT XplBool WJERename(WJElement document, const char *name)
{
	_WJElement	*current = (_WJElement *) document;
	WJElement	e;

	if (!document) {
		return(FALSE);
	}

	/* Look for any siblings with that name, and fail if found */
	if (name && document->parent) {
		for (e = document->parent->child; e; e = e->next) {
			if (e != document && !stricmp(e->name, name)) {
				return(FALSE);
			}
		}
	}

	/* Free the previous name if needed */
	if (document->name && current->_name != document->name) {
		MemRelease(&document->name);
	}

	/* Set the new name */
	if (name) {
		if (!(document->name = MemStrdup(name))) {
			return(FALSE);
		}
	} else {
		document->name = NULL;
	}

	return(TRUE);
}

static WJElement _WJELoad(_WJElement *parent, WJReader reader, char *where, WJELoadCB loadcb, void *data, const char *file, const int line)
{
	char		*current, *name, *value;
	_WJElement	*l = NULL;
	XplBool		complete;
	size_t		actual, used, len;

	if (!reader) {
		return((WJElement) _WJENew(NULL, NULL, 0, file, line));
	}

	if (!where) {
		/*
			A NULL poisition in a WJReader indicates the root of the document,
			so we must read to find the first real object.
		*/
		where = WJRNext(NULL, 256, reader);
	}

	if (!where) {
		/* This appears to be an empty document */
		return(NULL);
	}

	name = where[1] ? where + 1 : NULL;
	if (loadcb && !loadcb((WJElement)parent, name, data, file, line)) {
		/* The consumer has rejected this item */
		return(NULL);
	}

	if ((l = _WJENew(parent, name, name ? strlen(name) : 0, file, line))) {
		switch ((l->pub.type = *where)) {
			default:
			case WJR_TYPE_UNKNOWN:
			case WJR_TYPE_NULL:
				break;

			case WJR_TYPE_OBJECT:
			case WJR_TYPE_ARRAY:
				while (reader && (current = WJRNext(where, 256, reader))) {
					_WJELoad(l, reader, current, loadcb, data, file, line);
				}
				break;

			case WJR_TYPE_STRING:
				actual			= 0;
				used			= 0;
				complete		= FALSE;
				l->value.string	= NULL;

				do {
					if ((value = WJRString(&complete, reader))) {
						len = strlen(value);
						if (used + len >= actual) {
							l->value.string = MemMallocEx(l->value.string,
								len + 1 + used, &actual, TRUE, TRUE);
							MemUpdateOwner(l->value.string, file, line);
						}

						memcpy(l->value.string + used, value, len);
						used += len;
						l->value.string[used] = '\0';
					}
				} while (!complete);
				break;

			case WJR_TYPE_NUMBER:
				if (WJRNegative(reader)) {
					/*
						Store the number as a positive, but keep track of the
						fact that it was negative.
					*/
					l->negative = TRUE;
					l->value.number = (-WJRInt64(reader));
				} else {
					l->negative = FALSE;
					l->value.number = WJRUInt64(reader);
				}
				break;

			case WJR_TYPE_TRUE:
				l->value.boolean = TRUE;
				break;

			case WJR_TYPE_BOOL:
			case WJR_TYPE_FALSE:
				l->value.boolean = FALSE;
				break;
		}
	}

	return((WJElement) l);
}

EXPORT WJElement _WJEOpenDocument(WJReader reader, char *where, WJELoadCB loadcb, void *data, const char *file, const int line)
{
	WJElement	element;

	if ((element = _WJELoad(NULL, reader, where, loadcb, data, file, line))) {
		MemUpdateOwner(element, file, line);
	}

	return(element);
}

static WJElement _WJECopy(_WJElement *parent, WJElement original, WJELoadCB loadcb, void *data, const char *file, const int line)
{
	_WJElement	*l = NULL;
	WJElement	c;

	if (!original) {
		return(NULL);
	}

	if (loadcb && !loadcb((WJElement) parent, original->name, data, file, line)) {
		/* The consumer has rejected this item */
		return(NULL);
	}

	if ((l = _WJENew(parent, original->name, original->name ? strlen(original->name) : 0, file, line))) {
		switch ((l->pub.type = original->type)) {
			default:
			case WJR_TYPE_UNKNOWN:
			case WJR_TYPE_NULL:
				break;

			case WJR_TYPE_OBJECT:
			case WJR_TYPE_ARRAY:
				for (c = original->child; c; c = c->next) {
					_WJECopy(l, c, loadcb, data, file, line);
				}
				break;

			case WJR_TYPE_STRING:
				l->value.string = MemStrdup(WJEString(original, NULL, WJE_GET, ""));
				break;

			case WJR_TYPE_NUMBER:
				l->value.number = WJENumber(original, NULL, WJE_GET, 0);
				break;

			case WJR_TYPE_TRUE:
			case WJR_TYPE_BOOL:
			case WJR_TYPE_FALSE:
				l->value.boolean = WJEBool(original, NULL, WJE_GET, FALSE);
				break;
		}
	}

	return((WJElement) l);
}

EXPORT WJElement _WJECopyDocument(WJElement to, WJElement from, WJELoadCB loadcb, void *data, const char *file, const int line)
{
	if (to) {
		WJElement	c;

		for (c = from->child; c; c = c->next) {
			_WJECopy((_WJElement *) to, c, loadcb, data, file, line);
		}
	} else {
		if ((to = _WJECopy(NULL, from, loadcb, data, file, line))) {
			MemUpdateOwner(to, file, line);
		}
	}

	return(to);
}

EXPORT XplBool WJEMergeObjects(WJElement to, WJElement from, XplBool overwrite)
{
	WJElement	a, b;

	if (!to || !from ||
		WJR_TYPE_OBJECT != to->type || WJR_TYPE_OBJECT != from->type
	) {
		return(FALSE);
	}

	for (a = from->child; a; a = a->next) {
		if ((b = WJEChild(to, a->name, WJE_GET))) {
			if (WJR_TYPE_OBJECT == b->type && WJR_TYPE_OBJECT == a->type) {
				/* Merge all the children */
				WJEMergeObjects(b, a, overwrite);

				continue;
			} else if (overwrite) {
				/* Remove the existing value and overwrite it */
				WJECloseDocument(b);
			} else {
				/* Do nothing with this element */
				continue;
			}
		}

		/* Copy the object over */
		WJEAttach(to, WJECopyDocument(NULL, a, NULL, NULL));
	}

	return(TRUE);
}

EXPORT XplBool WJEWriteDocument(WJElement document, WJWriter writer, char *name)
{
	_WJElement	*current = (_WJElement *) document;
	WJElement	child;

	if (!document) {
		return(FALSE);
	}

	switch (current->pub.type) {
		default:
		case WJR_TYPE_UNKNOWN:
			break;

		case WJR_TYPE_NULL:
			WJWNull(name, writer);
			break;

		case WJR_TYPE_OBJECT:
			WJWOpenObject(name, writer);

			for (child = current->pub.child; child; child = child->next) {
				WJEWriteDocument(child, writer, child->name);
			}

			WJWCloseObject(writer);
			break;

		case WJR_TYPE_ARRAY:
			WJWOpenArray(name, writer);

			for (child = current->pub.child; child; child = child->next) {
				WJEWriteDocument(child, writer, child->name);
			}

			WJWCloseArray(writer);
			break;

		case WJR_TYPE_STRING:
			WJWString(name, current->value.string, TRUE, writer);
			break;

		case WJR_TYPE_NUMBER:
			if (!current->negative) {
				WJWUInt64(name, current->value.number, writer);
			} else {
				WJWInt64(name, current->value.number, writer);
			}
			break;

		case WJR_TYPE_TRUE:
		case WJR_TYPE_BOOL:
		case WJR_TYPE_FALSE:
			WJWBoolean(name, current->value.boolean, writer);
			break;
	}

	return(TRUE);
}

EXPORT XplBool WJECloseDocument(WJElement document)
{
	_WJElement	*current = (_WJElement *) document;

	if (!document) {
		return(FALSE);
	}

	if (document->freecb && !document->freecb(document)) {
		/* The callback has prevented free'ing the document */
		return(TRUE);
	}

	WJEChanged(document);

	/* Remove references to this object */
	if (document->parent) {
		if (document->parent->child == document) {
			document->parent->child = document->next;
		}
		document->parent->count--;
	}

	if (document->prev) {
		document->prev->next = document->next;
	}

	if (document->next) {
		document->next->prev = document->prev;
	}


	/* Destroy all children */
	while (document->child) {
		WJECloseDocument(document->child);
	}

	if (current->pub.type == WJR_TYPE_STRING) {
		MemFree(current->value.string);
	}

	if (document->name && current->_name != document->name) {
		MemRelease(&document->name);
	}

	MemFree(current);

	return(TRUE);
}

EXPORT void WJEDump(WJElement document)
{
	WJWriter		dumpWriter;

	if ((dumpWriter = WJWOpenFILEDocument(TRUE, stdout))) {
		WJEWriteDocument(document, dumpWriter, NULL);
		WJWCloseDocument(dumpWriter);
	}
	fprintf(stdout, "\n");
}
