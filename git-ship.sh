#!/usr/bin/env bash

# 1. Pre-check: Are there local changes?
if [[ -n $(git status --porcelain) ]]; then
    echo "Error: You have uncommitted changes in your working directory. Stash or commit them first."
    exit 1
fi

# 2. Pre-check: Is 'main' ahead of local?
git fetch github
LOCAL_MAIN=$(git rev-parse main)
REMOTE_MAIN=$(git rev-parse github/main)

if [[ "$LOCAL_MAIN" != "$REMOTE_MAIN" ]]; then
    echo "Error: GitHub's 'main' has changes that you do not have locally."
    echo "Please pull and manually integrate those changes into your 'dev' branch first."
    exit 1
fi

# 3. Perform the squash merge
echo "Merging dev into main..."
git switch main
git merge --squash dev --allow-unrelated-histories

# 4. Commit and Push
if [[ $? -eq 0 ]]; then
    git commit
    
    echo "Pushing to GitHub..."
    git push github main
    
    echo "Pushing granular history to Gitea..."
    git push gitea dev
else
    echo "Merge failed! Check for conflicts."
    exit 1
fi

# 5. Return to dev
git switch dev