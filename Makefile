#
#  oooo  oooo
#  `888  `888
#   888   888  ooo. .oo.  .oo.    .ooooo oo
#   888   888  `888P"Y88bP"Y88b  d88' `888
#   888   888   888   888   888  888   888
#  o888o o888o o888o o888o o888o `V8bod888
#                                      888.
#  a query CLI, plugin framework, and  8P'
#  I/O manager for conversational AIs  "
#
#  Copyright (C) 2023 Justin Collier <m@jpcx.dev>
#
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Affero General Public License as
#    published by the Free Software Foundation, either version 3 of the
#    License, or (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.

PROGRAM  = llmq
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

CXXFLAGS += $(shell pkg-config --cflags libcurl)       \
	    -std=c++20 -Wall -Wextra -pedantic -Werror \
	    -O3 -I. -fmax-errors=1
LDFLAGS  += $(shell pkg-config --libs libcurl)

SOURCES = llmq.cc 3rdparty/ryml.cc $(wildcard plugins/*.cc)
OBJECTS = $(patsubst %.cc,.build/%.o,$(SOURCES))

-include $(wildcard plugins/*.mk)

all: $(PROGRAM)

.build/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@ $(LDFLAGS)

$(PROGRAM): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

install: $(PROGRAM)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)
	install -m 0755 $(PROGRAM) $(DESTDIR)$(BINDIR)/$(PROGRAM)
	install -m 0644 $(PROGRAM).1 $(DESTDIR)$(MANDIR)/$(PROGRAM).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROGRAM)
	rm -f $(DESTDIR)$(MANDIR)/$(PROGRAM).1

clean:
	$(RM)    $(PROGRAM)
	$(RM) -r .build

.PHONY: all clean
