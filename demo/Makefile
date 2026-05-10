# pset4/demo/Makefile - demo orchestration.

WORK ?= $(CURDIR)/work
PORT ?= 8080
TM_URL ?= http://localhost:$(PORT)

TM_LOG ?= $(CURDIR)/../tm-server.log
SNAPSHOTS ?= $(CURDIR)/../snapshots.json

.PHONY: init clean server seed status help viz replay

help:
	@echo "Targets:"
	@echo "  make init      Wipe and reseed the bare repo + start tm-server"
	@echo "  make seed      Post the root plan task to the running tm-server"
	@echo "  make status    Pretty-print the current task table"
	@echo "  make replay    Run tm-replay on tm-server.log -> snapshots.json"
	@echo "  make viz       Replay then start the web visualizer (Flask)"
	@echo "  make clean     Stop tm-server, remove work/"
	@echo ""
	@echo "Three-Claude run:"
	@echo "  Terminal 1: make init  (then leave running)"
	@echo "  Terminals 2,3,4: cd $(WORK)/agent-N && claude  → /task-loop"

init: clean
	mkdir -p $(WORK)
	git init --bare $(WORK)/repo.git
	git clone $(WORK)/repo.git $(WORK)/seed
	cd $(WORK)/seed && \
	    git -c user.email=human@demo -c user.name=human commit \
	        --allow-empty -m "initial commit" && \
	    git push origin master:main
	mkdir -p $(WORK)/agent-1 $(WORK)/agent-2 $(WORK)/agent-3
	@echo ""
	@echo "Now run in another terminal:"
	@echo "  cd $(CURDIR)/.. && ./build/tm-server -V -p $(PORT)"
	@echo "Then 'make seed' to post the root plan task."

seed:
	curl -fsS -X POST $(TM_URL)/task_create -d @seed-task.json | jq .

status:
	curl -fsS $(TM_URL)/dump | jq '.tasks | map({id, type:.spec.type, status, owner_agent, title:.spec.title})'

replay:
	$(CURDIR)/../build/tm-replay -L $(TM_LOG) -o $(SNAPSHOTS)

PYTHON ?= python3
viz: replay
	cd $(CURDIR)/.. && $(PYTHON) visualize_tm.py --snapshots $(SNAPSHOTS)

clean:
	rm -rf $(WORK)
