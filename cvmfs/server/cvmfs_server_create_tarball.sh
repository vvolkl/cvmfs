#
# This file is part of the CernVM File System
# This script takes care of creating, removing, and maintaining repositories
# on a Stratum 0/1 server
#
# Implementation of the "cvmfs_server create-tarball" command

# This file depends on functions implemented in the following files:
# - cvmfs_server_util.sh
# - cvmfs_server_common.sh

cvmfs_server_create_tarball() {
  local repo_subpath=""
  local output_tarball=""
  local temp_dir=""
  local name

  # optional parameter handling
  OPTIND=1
  while getopts "p:o:l:" option
  do
    case $option in
      p)
        repo_subpath="$OPTARG"
      ;;
      o)
        output_tarball="$OPTARG"
      ;;
      l)
        temp_dir="$OPTARG"
      ;;
      ?)
        shift $(($OPTIND-2))
        usage "Command create-tarball: Unrecognized option: $1"
      ;;
    esac
  done

  # get repository name
  shift $(($OPTIND-1))
  check_parameter_count_with_guessing $#
  name=$(get_or_guess_repository_name $1)

  [ x"$repo_subpath" != x"" ] || die "Please provide a repository subpath with -p"
  [ x"$output_tarball" != x"" ] || die "Please provide an output tarball path with -o"

  # sanity checks
  check_repository_existence $name || die "The repository $name does not exist"

  # get repository information
  load_repo_config $name

  # more sanity checks
  is_owner_or_root $name || die "Permission denied: Repository $name is owned by $CVMFS_USER"
  health_check     $name || die "Repository $name is not healthy"

  # check if repository is compatible to the installed CernVM-FS version
  check_repository_compatibility $name

  is_publishing $name && die "Another publish process is active for $name"
  is_in_transaction $name && die "Cannot create a tarball while $name is in a transaction; publish or abort first"

  local root_hash=$(get_mounted_root_hash $name)
  [ x"$root_hash" != x"" ] || die "Failed to determine the current repository root hash for $name"

  local repo_url="$CVMFS_STRATUM0"
  [ x"$repo_url" != x"" ] || die "Repository $name does not define CVMFS_STRATUM0"
  [ x"$CVMFS_PUBLIC_KEY" != x"" ] || die "Repository $name does not define CVMFS_PUBLIC_KEY"
  cvmfs_sys_file_is_regular "$CVMFS_PUBLIC_KEY" || die "Repository public key $CVMFS_PUBLIC_KEY does not exist"

  if [ x"$temp_dir" = x"" ]; then
    temp_dir="${CVMFS_SPOOL_DIR}/tmp"
  fi
  [ -d "$temp_dir" ] || die "Temporary directory $temp_dir does not exist"

  local user_shell="$(get_user_shell $name)"
  [ x"$user_shell" != x"" ] || die "Failed to determine how to run commands as $CVMFS_USER"

  local create_tarball_command="$(__swissknife_cmd dbg) create-tarball \
    -r \"$repo_url\"                                                 \
    -n \"$CVMFS_REPOSITORY_NAME\"                                    \
    -k \"$CVMFS_PUBLIC_KEY\"                                         \
    -h \"$root_hash\"                                                \
    -l \"$temp_dir\"                                                 \
    -p \"$repo_subpath\"                                             \
    -o \"$output_tarball\"                                           \
    $(get_swissknife_proxy)                                             \
    $(get_follow_http_redirects_flag)"

  $user_shell "$create_tarball_command"
  local retval=$?
  [ $retval -eq 0 ] || die "Create-tarball failed\n\nExecuted Command:\n$create_tarball_command"
}
