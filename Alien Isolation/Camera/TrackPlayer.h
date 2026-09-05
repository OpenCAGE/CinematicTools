#pragma once
#include "CameraStructs.h"
#include <Model.h>
#include <memory>
#include <vector>

// Camera tracks: a list of nodes captured from the free camera, played back along a Catmull-Rom spline.
// Time between nodes is set when a node is created; playback runs in real time, manually (scrubbed with
// the camera up/down inputs), or from the timeline in the UI.
class TrackPlayer
{
public:
  TrackPlayer();
  ~TrackPlayer();

  // Appends a node captured from the camera, m_NodeTimeSpan seconds after the previous one.
  void CreateNode(Camera const& camera);
  // Removes the last node.
  void DeleteNode();
  // Removes a node, closing the gap in time it leaves behind.
  void DeleteNode(size_t index);
  // Overwrites a node with the camera's current state, keeping its time.
  void UpdateNode(size_t index, Camera const& camera);
  // Moves a node in time, kept between its neighbours.
  void SetNodeTime(size_t index, float time);

  void Toggle();
  void Play();
  void Stop();

  // Advances playback and returns the camera state for the new time.
  CatmullRomNode PlayForward(float dt, bool ignoreManual = false);
  CatmullRomNode PlayForwardSmooth(float dt, bool ignoreManual = false);

  // Jumps playback to a time without moving the camera.
  void Seek(float time);
  // The camera state at a time on the track, leaving playback where it was.
  CatmullRomNode Evaluate(float time);

  // Track list, node timing and playback options.
  void DrawUI();
  // In-world preview of the nodes and the path between them.
  void DrawNodes();

  bool IsPlaying() { return m_IsPlaying; }
  bool IsRotationLocked() { return m_LockRotation; }
  bool IsFovLocked() { return m_LockFieldOfView; }
  bool IsDofLocked() { return m_LockDepthOfField; }
  bool IsLooping() { return m_Loop; }
  bool IsTrackVisible() { return m_ShowTrack; }

  float GetTime() { return m_CurrentTime; }
  float GetDuration();
  std::vector<CatmullRomNode> const& GetNodes() { return m_Tracks[m_SelectedTrack].Nodes; }

private:
  void CreateTrack();
  void DeleteTrack();

  void UpdateNodeBuffers();
  void UpdateNameList();

  void SmoothTrack();

  // Finds the node segment and smooth node containing the current time.
  void FindCursors();
  // The state at a node, for the ends of the track.
  CatmullRomNode NodeState(CatmullRomNode const& node);

private:
  bool m_IsPlaying;

  bool m_LockDepthOfField;
  bool m_LockRotation;
  bool m_LockFieldOfView;
  bool m_ManualPlay;
  bool m_Loop;
  bool m_ShowTrack;
  float m_NodeTimeSpan;

  unsigned int m_CurrentNode;
  unsigned int m_CurrentSmoothNode;
  float m_CurrentTime;

  std::vector<CameraTrack> m_Tracks;
  unsigned int m_SelectedTrack;

  std::vector<const char*> m_TrackNames;
  int m_RunningId;

  std::unique_ptr<DirectX::Model> m_pCameraModel;

public:
  TrackPlayer(TrackPlayer const&) = delete;
  void operator=(TrackPlayer const&) = delete;

};
