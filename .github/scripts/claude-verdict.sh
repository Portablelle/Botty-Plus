#!/usr/bin/env bash
# Whether Claude's review clears pull request $1 at its head. Prints the reason (one markdown sentence, no
# call to action: the callers add it) and exits:
#   0  cleared: the review's verdict=pass is on the head, or a current-head maintainer review authorizes the claude-review-override label;
#   1  blocked by open findings or failing checks, which a fix clears;
#   2  blocked without a verdict on the head: review running or failed, fork PR, a PR that changes the
#      review workflow (the action skips those), or a review from before verdicts existed.
# Used by the review workflow's verdict. Needs GH_TOKEN and REPO.
set -euo pipefail
pr=$1

info=$(gh api "repos/$REPO/pulls/$pr")
head=$(jq -r .head.sha <<<"$info")
fork=$(jq -r --arg repo "$REPO" '.head.repo.full_name != $repo' <<<"$info")
override=$(jq -r 'any(.labels[]; .name == "claude-review-override")' <<<"$info")

# Only the review bot's summary counts: anyone can paste the marker into a comment.
summary=$(gh api "repos/$REPO/issues/$pr/comments" --paginate \
  --jq '.[] | select(.user.login == "claude[bot]" and (.body | startswith("<!-- claude-review ")))' | jq -s 'last // {}')
marker=$(jq -r '.body // "" | split("\n")[0]' <<<"$summary")
field() { sed -nE "s/.* $1=([^ ]+).*/\\1/p" <<<"$marker"; }
sha=$(field sha) verdict=$(field verdict) open=$(field open)
[ "$open" = none ] && open=
review=$(jq -r '.html_url // empty | "[Claude'"'"'s review](\(.))"' <<<"$summary")
review=${review:-Claude\'s review}

# A label alone is not bound to a commit and survives fork pushes/reopens.
# A maintainer must leave a commented review on this exact head with the first
# line `claude-review-override`, then add the label. The review records commit_id.
cleared_override=false
if [ "$override" = true ]; then
  reviewers=$(gh api "repos/$REPO/pulls/$pr/reviews" --paginate \
    --jq '.[] | select(.commit_id == "'"$head"'" and .state != "DISMISSED" and ((.body // "") | split("\n")[0]) == "claude-review-override") | .user.login')
  for login in $reviewers; do
    permission=$(gh api "repos/$REPO/collaborators/$login/permission" --jq .permission) || continue
    case "$permission" in
      admin|maintain|write) cleared_override=true; break ;;
    esac
  done
fi

if [ "$sha" = "$head" ] && [ "$verdict" = pass ]; then
  echo "$review of \`${head::7}\` found nothing to fix."
elif [ "$cleared_override" = true ]; then
  echo "A maintainer authorized \`claude-review-override\` on this exact head."
elif [ "$fork" = true ]; then
  echo "Fork PRs get no automatic review."
  exit 2
elif [ "$sha" != "$head" ] || [ "$verdict" != block ]; then
  echo "There is no verdict from Claude's review on \`${head::7}\`."
  exit 2
elif [ -n "$open" ]; then
  echo "$review of \`${head::7}\` left ${open//,/, } to fix."
  exit 1
else
  echo "$review of \`${head::7}\` found failing host checks."
  exit 1
fi
