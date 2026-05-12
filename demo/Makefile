# pset4/demo/Makefile - demo orchestration.
#
# Two demo modes:
#   - single-instance (legacy POC): make init/seed/status/replay/viz
#   - 3-replica paxos (Phase 4):    make demo-up/demo-init/demo-status/...
#
# The 3-replica targets shell out to `docker compose` (compose v2) and
# expect to run from the macOS host (or wherever docker-cli is available).
# Inter-replica networking is via the compose network `pset4demo_default`.

WORK ?= $(CURDIR)/work
PORT ?= 8080
TM_URL ?= http://localhost:$(PORT)

TM_LOG ?= $(CURDIR)/../tm-server.log
SNAPSHOTS ?= $(CURDIR)/../snapshots.json

# 3-replica demo: HTTP ports exposed on host, paxos TCP stays internal.
DEMO_R0 ?= http://localhost:8081
DEMO_R1 ?= http://localhost:8082
DEMO_R2 ?= http://localhost:8083
COMPOSE ?= docker compose
COMPOSE_FILE ?= $(CURDIR)/../docker-compose.yml
COMPOSE_NET ?= pset4demo_default

.PHONY: init clean server seed status help viz replay \
        demo-help demo-build demo-up demo-down demo-deploy demo-seed demo-init \
        demo-status demo-capture demo-replay-verify

help:
	@echo "Single-instance (POC) targets:"
	@echo "  make init      Wipe and reseed the bare repo + start tm-server"
	@echo "  make seed      Post the root plan task to the running tm-server"
	@echo "  make status    Pretty-print the current task table"
	@echo "  make replay    Run tm-replay on tm-server.log -> snapshots.json"
	@echo "  make viz       Replay then start the web visualizer (Flask)"
	@echo "  make clean     Stop tm-server, remove work/"
	@echo ""
	@echo "3-replica paxos demo (Phase 4):"
	@echo "  make demo-help"
	@echo ""
	@echo "Three-Claude run (single-instance):"
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

# ====== 3-replica paxos demo (Phase 4) ======

demo-help:
	@echo "3-replica paxos demo (runs on host via docker compose):"
	@echo ""
	@echo "  make demo-build           Compile tm-server inside cs2620:latest"
	@echo "                            (one-time / after source changes)"
	@echo "  make demo-up              Start 3 replicas (replica-0/1/2)"
	@echo "  make demo-down            Stop and remove all 3"
	@echo "  make demo-deploy          Create demo/work/repo.git + agent-{1,2,3}"
	@echo "                            via the swarm-deploy skill"
	@echo "  make demo-seed            POST the root plan task (waits for /dump)"
	@echo "  make demo-init            Composite: deploy + up + seed"
	@echo "  make demo-status          Pretty-print task table from replica-0"
	@echo ""
	@echo "Failure injection:"
	@echo "  make demo-pause-N         SIGSTOP replica-N (recovery story)"
	@echo "  make demo-resume-N        SIGCONT replica-N"
	@echo "  make demo-kill-N          SIGKILL replica-N (survival story; no auto-restart)"
	@echo "  make demo-partition-N     Disconnect replica-N from compose network"
	@echo "  make demo-reconnect-N     Reconnect replica-N"
	@echo ""
	@echo "After a demo run:"
	@echo "  make demo-capture DIR=demo/run-1   Bundle logs + final state"
	@echo "  make demo-replay-verify            Diff snapshots from 3 logs"

# Build tm-server inside cs2620:latest so the binary in build/ is Linux ELF.
# Annie typically builds inside her ../run-docker shell instead — this target
# is the docker-compose equivalent for users not in that workflow.
demo-build:
	$(COMPOSE) -f $(COMPOSE_FILE) run --rm --entrypoint='' replica-0 \
	    make -C /work/pset4-active

demo-up:
	$(COMPOSE) -f $(COMPOSE_FILE) up -d

demo-down:
	$(COMPOSE) -f $(COMPOSE_FILE) down

# Run the swarm-deploy skill from the host. Creates demo/work/repo.git (bare)
# and demo/work/agent-{1,2,3} dirs with templated CLAUDE.md files. Then
# post-process each CLAUDE.md to pin TM_URL to a different replica:
# agent-1 → :8081 (replica-0), agent-2 → :8082 (replica-1), agent-3 → :8083 (replica-2).
# 307 redirects handle the case where the pinned replica isn't the leader.
demo-deploy:
	mkdir -p $(WORK)
	$(CURDIR)/../.claude/skills/swarm-deploy/scripts/deploy.py $(WORK) --force
	@for i in 1 2 3; do \
	    port=$$((8080 + i)); \
	    sed -i.bak "s|http://localhost:8080|http://localhost:$$port|g" $(WORK)/agent-$$i/CLAUDE.md; \
	    rm -f $(WORK)/agent-$$i/CLAUDE.md.bak; \
	    echo "  agent-$$i pinned to http://localhost:$$port"; \
	done

# Wait for paxos quorum, then POST the seed.
#
# The /dump check below only proves the HTTP server is alive — it doesn't
# touch paxos. After a rapid teardown+restart, /dump can answer before the
# inter-replica TCP mesh has reformed, so a seed POST issued at that
# moment blocks forever waiting for quorum. We poll task_list (read-only
# but goes through propose-and-apply) with a short per-try timeout to
# detect when quorum is actually ready. Then the seed gets a bounded
# --max-time so a stuck cluster fails loud instead of hanging.
demo-seed:
	@for i in $$(seq 1 30); do \
	    if curl -fsS $(DEMO_R0)/dump > /dev/null 2>&1; then \
	        echo "replica-0 HTTP ready"; break; \
	    fi; \
	    sleep 1; \
	done
	@for i in $$(seq 1 15); do \
	    if curl -fsS --max-time 2 -X POST $(DEMO_R0)/task_list \
	        -H 'Content-Type: application/json' \
	        -d '{"agent_id":"_readiness","serial":'$$i'}' \
	        > /dev/null 2>&1; then \
	        echo "paxos quorum ready"; break; \
	    fi; \
	    sleep 1; \
	done
	curl -fsSL --max-time 30 -X POST $(DEMO_R0)/task_create \
	    -H 'Content-Type: application/json' \
	    -d @$(CURDIR)/seed-task.json | jq .

demo-init: demo-deploy demo-up demo-seed

demo-status:
	curl -fsSL $(DEMO_R0)/dump | jq '.tasks | map({id, type:.spec.type, status, owner_agent, title:.spec.title})'

# Failure injection — pattern targets so `make demo-pause-1` works.
demo-pause-%:
	$(COMPOSE) -f $(COMPOSE_FILE) pause replica-$*

demo-resume-%:
	$(COMPOSE) -f $(COMPOSE_FILE) unpause replica-$*

demo-kill-%:
	$(COMPOSE) -f $(COMPOSE_FILE) kill replica-$*

demo-partition-%:
	docker network disconnect $(COMPOSE_NET) pset4demo-replica-$*-1 || \
	docker network disconnect $(COMPOSE_NET) replica-$*

# --alias re-registers the service name in compose's embedded DNS.
# Without it, `docker network connect` re-attaches the container at a new
# IP but `replica-N` stays NXDOMAIN for peers, so paxos can't reconnect.
demo-reconnect-%:
	docker network connect --alias replica-$* $(COMPOSE_NET) pset4demo-replica-$*-1 || \
	docker network connect --alias replica-$* $(COMPOSE_NET) replica-$*

# Bundle decision logs, compose stderr, and final dumps into DIR for writeup.
demo-capture:
	@test -n "$(DIR)" || (echo "Usage: make demo-capture DIR=demo/run-N"; exit 1)
	mkdir -p $(DIR)
	cp $(CURDIR)/../logs/tm-0.log $(DIR)/ 2>/dev/null || true
	cp $(CURDIR)/../logs/tm-1.log $(DIR)/ 2>/dev/null || true
	cp $(CURDIR)/../logs/tm-2.log $(DIR)/ 2>/dev/null || true
	$(COMPOSE) -f $(COMPOSE_FILE) logs --no-color > $(DIR)/compose-stderr.log 2>&1 || true
	curl -fsSL $(DEMO_R0)/dump > $(DIR)/dump-replica-0.json 2>/dev/null || true
	curl -fsSL $(DEMO_R1)/dump > $(DIR)/dump-replica-1.json 2>/dev/null || true
	curl -fsSL $(DEMO_R2)/dump > $(DIR)/dump-replica-2.json 2>/dev/null || true
	@echo "captured to $(DIR)/"

# Replay each replica's decision log and verify identical snapshots — the
# real-paxos correctness invariant ("3 replicas, 1 timeline"). Runs
# tm-replay inside a replica container so the Linux binary works from
# a macOS host. Requires `make demo-up` (or demo-init) to have run.
demo-replay-verify:
	@for i in 0 1 2; do \
	    $(COMPOSE) -f $(COMPOSE_FILE) exec -T replica-0 \
	        ./build/tm-replay -L logs/tm-$$i.log -o /tmp/snapshots-$$i.json \
	        2>/dev/null || true; \
	    $(COMPOSE) -f $(COMPOSE_FILE) exec -T replica-0 \
	        cat /tmp/snapshots-$$i.json > $(CURDIR)/../logs/snapshots-$$i.json 2>/dev/null || true; \
	done
	@if diff -q $(CURDIR)/../logs/snapshots-0.json $(CURDIR)/../logs/snapshots-1.json > /dev/null && \
	    diff -q $(CURDIR)/../logs/snapshots-1.json $(CURDIR)/../logs/snapshots-2.json > /dev/null; then \
	    echo "PASS: all 3 replicas replay to identical snapshots"; \
	else \
	    echo "FAIL: replica replays differ"; \
	    diff $(CURDIR)/../logs/snapshots-0.json $(CURDIR)/../logs/snapshots-1.json | head -20; \
	fi
