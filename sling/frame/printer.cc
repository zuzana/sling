// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sling/frame/printer.h"

#include <string>

#include "sling/base/logging.h"
#include "sling/frame/store.h"
#include "sling/string/ctype.h"
#include "sling/string/numbers.h"

namespace sling {

void Printer::Print(const Object &object) {
  CHECK(object.store() == nullptr ||
        object.store() == store_ ||
        object.store() == store_->globals());
  Print(object.handle());
}

void Printer::Print(Handle handle, bool reference) {
  if (handle.IsNil()) {
    output_->Write("nil");
  } else if (handle.IsRef()) {
    const Datum *datum = store_->GetObject(handle);
    switch (datum->type()) {
      case STRING:
        PrintString(datum->AsString());
        break;

      case FRAME:
        PrintFrame(datum->AsFrame());
        break;

      case SYMBOL:
        PrintSymbol(datum->AsSymbol(), reference);
        break;

      case ARRAY:
        PrintArray(datum->AsArray());
        break;

      case INVALID:
        output_->Write("<<<invalid object>>>");
        break;

      default:
        output_->Write("<<<unknown object type>>>");
    }
  } else if (handle.IsInt()) {
    PrintInt(handle.AsInt());
  } else if (handle.IsFloat()) {
    if (handle.IsIndex()) {
      WriteChar(reference ? '#' : '@');
      PrintInt(handle.AsIndex());
    } else {
      PrintFloat(handle.AsFloat());
    }
  } else {
    output_->Write("<<<unknown handle type>>>");
  }
}

void Printer::PrintAll() {
  const MapDatum *map = store_->GetMap(store_->symbols());
  for (Handle *bucket = map->begin(); bucket < map->end(); ++bucket) {
    Handle h = *bucket;
    while (!h.IsNil()) {
      const SymbolDatum *symbol = store_->GetSymbol(h);
      if (symbol->bound() && !store_->IsProxy(symbol->value)) {
        Print(symbol->value);
        WriteChar('\n');
      }
      h = symbol->next;
    }
  }
}

void Printer::PrintString(const StringDatum *str) {
  // Escape types.
  enum Escaping {NONE, NEWLINE, RETURN, TAB, QUOTE, BSLASH, HEX};

  // Escape type for each character.
  static Escaping escaping[256] = {
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x00
    HEX, TAB, NEWLINE, HEX, HEX, RETURN, HEX, HEX,     // 0x08
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x10
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x18
    NONE, NONE, QUOTE, NONE, NONE, NONE, NONE, NONE,   // 0x20
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x28
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x30
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x38
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x40
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x48
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x50
    NONE, NONE, NONE, NONE, BSLASH, NONE, NONE, NONE,  // 0x58
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x60
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x68
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,    // 0x70
    NONE, NONE, NONE, NONE, NONE, NONE, NONE, HEX,     // 0x78
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x80
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x88
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x90
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0x98
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xA0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xA8
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xB0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xB8
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xC0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xC8
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xD0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xD8
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xE0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xE8
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xF0
    HEX, HEX, HEX, HEX, HEX, HEX, HEX, HEX,            // 0xF8
  };

  // Hexadecimal digits.
  static char hexdigit[] = "0123456789abcdef";

  WriteChar('"');
  bool done = false;
  unsigned char *s = str->payload();
  unsigned char *end = str->limit();
  while (!done) {
    // Search forward until first character that needs escaping.
    unsigned char *t = s;
    Escaping escape = NONE;
    while (t != end) {
      escape = escaping[*t];
      if (escape != NONE) break;
      t++;
    }

    // Output all characters before the escaped character.
    if (t != s) output_->Write(reinterpret_cast<char *>(s), t - s);

    // Escape character.
    switch (escape) {
      case NONE: done = true; break;
      case NEWLINE: WriteChars('\\', 'n'); t++; break;
      case RETURN: WriteChars('\\', 'r'); t++; break;
      case TAB: WriteChars('\\', 't'); t++; break;
      case QUOTE: WriteChars('\\', '"'); t++; break;
      case BSLASH: WriteChars('\\', '\\'); t++; break;
      case HEX:
        WriteChars('\\', 'x');
        WriteChar(hexdigit[*t >> 4]);
        WriteChar(hexdigit[*t & 0x0f]);
        t++;
        break;
    }
    s = t;
  }
  WriteChar('"');
}

void Printer::PrintFrame(const FrameDatum *frame) {
  // If frame has already been printed, only print a reference.
  Handle &ref = references_[frame->self];
  if (!ref.IsNil()) {
    if (byref_ || !ref.IsIndex()) {
      Print(ref, true);
      return;
    }
  }

  // Increase indentation for nested frames.
  WriteChar('{');
  if (indent()) current_indentation_ += indent_;

  // Add frame to set of printed references.
  if (frame->IsAnonymous()) {
    // Assign next local id and encode it as an index reference.
    Handle id = Handle::Index(next_index_++);
    ref = id;

    // Output index reference for anonymous frame.
    if (byref_) {
      WriteChar('=');
      Print(id, true);
      WriteChar(' ');
    }
  } else {
    // Update reference table with frame id.
    ref = frame->get(Handle::id());
  }

  // Output slots.
  bool first = true;
  for (const Slot *slot = frame->begin(); slot < frame->end(); ++slot) {
    if (!indent() && !first) WriteChar(' ');
    if (indent()) {
      WriteChar('\n');
      for (int i = 0; i < current_indentation_; ++i) WriteChar(' ');
    }

    if (slot->name.IsId()) {
      WriteChar('=');
      Print(slot->value, true);
    } else if (slot->name.IsIsA()) {
      WriteChar(':');
      PrintLink(slot->value);
    } else if (slot->name.IsIs()) {
      WriteChar('+');
      PrintLink(slot->value);
    } else if (slot->name.IsNil()) {
      PrintLink(slot->value);
    } else {
      PrintLink(slot->name);
      WriteChars(':', ' ');
      PrintLink(slot->value);
    }

    first = false;
  }

  if (indent()) {
    // Restore indentation.
    current_indentation_ -= indent_;
    if (frame->begin() != frame->end()) {
      WriteChar('\n');
      for (int i = 0; i < current_indentation_; ++i) WriteChar(' ');
    }
  }
  WriteChar('}');
}

void Printer::PrintArray(const ArrayDatum *array) {
  WriteChar('[');
  bool first = true;
  for (Handle *element = array->begin(); element < array->end(); ++element) {
    if (!first) WriteChars(',', ' ');
    PrintLink(*element);
    first = false;
  }
  WriteChar(']');
}

void Printer::PrintSymbol(const SymbolDatum *symbol, bool reference) {
  if (!reference && symbol->bound()) WriteChar('\'');

  const StringDatum *name = store_->GetString(symbol->name);
  const char *p = name->data();
  const char *end = p + name->size();
  DCHECK(p != end);
  if (!ascii_isalpha(*p) && *p != '/' && *p != '_') WriteChar('\\');
  WriteChar(*p++);
  while (p < end) {
    char c = *p++;
    if (!ascii_isalnum(c) && c != '/' && c != '_' && c != '-') {
      WriteChar('\\');
    }
    WriteChar(c);
  }
}

void Printer::PrintLink(Handle handle) {
  // Determine if only a link to the object should be printed.
  if (handle.IsRef() && !handle.IsNil()) {
    const Datum *datum = store_->GetObject(handle);
    if (datum->IsFrame()) {
      if (datum->IsProxy()) {
        // Print unresolved symbol.
        const ProxyDatum *proxy = datum->AsProxy();
        Print(proxy->symbol, true);
        return;
      } else {
        const FrameDatum *frame = datum->AsFrame();
        if (frame->IsNamed()) {
          if (shallow_ || (!global_ && handle.IsGlobalRef())) {
            // Print reference.
            Print(frame->get(Handle::id()), true);
            return;
          }
        }
      }
    }
  }

  // Print value.
  Print(handle);
}

void Printer::PrintInt(int number) {
  char buffer[kFastToBufferSize];
  char *str = FastInt32ToBuffer(number, buffer);
  output_->Write(str, strlen(str));
}

void Printer::PrintFloat(float number) {
  char buffer[kFastToBufferSize];
  char *str = FloatToBuffer(number, buffer);
  output_->Write(str, strlen(str));
}

}  // namespace sling

