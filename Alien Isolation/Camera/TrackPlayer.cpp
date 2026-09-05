#include "TrackPlayer.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/ImGuiEXT.h"
#include "../resource.h"

#include <algorithm>

using namespace DirectX;

TrackPlayer::TrackPlayer() :
  m_IsPlaying(false),
  m_LockDepthOfField(false),
  m_LockRotation(true),
  m_LockFieldOfView(false),
  m_ManualPlay(false),
  m_Loop(false),
  m_ShowTrack(false),
  m_NodeTimeSpan(3.0f),
  m_CurrentNode(0),
  m_CurrentSmoothNode(0),
  m_CurrentTime(0),
  m_SelectedTrack(0),
  m_RunningId(2)
{
  m_Tracks.emplace_back("Track #1");
  m_TrackNames.push_back(m_Tracks[0].Name.c_str());

  m_pCameraModel = g_mainHandle->GetRenderer()->CreateModelFromResource(IDR_OBJ_CAMERA);
}

TrackPlayer::~TrackPlayer()
{

}

void TrackPlayer::CreateNode(Camera const& camera)
{
  if (m_IsPlaying) return;

  CatmullRomNode newNode;

  newNode.FieldOfView = camera.Profile.FieldOfView;
  newNode.FocusDistance = camera.Profile.FocusDistance;
  newNode.DofScale = camera.Profile.DofScale;
  newNode.DofStrength = camera.Profile.DofStrength;
  newNode.Rotation = camera.Rotation;
  newNode.Position = camera.Position;

  newNode.Transform = XMMatrixRotationQuaternion(XMLoadFloat4(&camera.Rotation));
  newNode.Transform.r[3] = XMLoadFloat3(&camera.Position);
  newNode.Transform.r[3].m128_f32[3] = 1.0f;

  newNode.TimeStamp = 0;

  size_t nodes = m_Tracks[m_SelectedTrack].Nodes.size();
  if (nodes > 0)
    newNode.TimeStamp = m_Tracks[m_SelectedTrack].Nodes[nodes - 1].TimeStamp + m_NodeTimeSpan;

  m_Tracks[m_SelectedTrack].Nodes.push_back(newNode);
  SmoothTrack();
  UpdateNodeBuffers();
  util::log::Write("Node created, total nodes: %d", m_Tracks[m_SelectedTrack].Nodes.size());
}

void TrackPlayer::DeleteNode()
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() == 0) return;

  DeleteNode(nodes.size() - 1);
}

void TrackPlayer::DeleteNode(size_t index)
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index >= nodes.size()) return;

  // Close the gap the node leaves: the following nodes move back by the span that preceded it
  // (or by the first node's offset when the first node goes, so the track still starts at 0).
  float shift = 0;
  if (index == 0 && nodes.size() > 1)
    shift = nodes[1].TimeStamp;
  else if (index > 0)
    shift = nodes[index].TimeStamp - nodes[index - 1].TimeStamp;

  nodes.erase(nodes.begin() + index);
  for (size_t i = index; i < nodes.size(); ++i)
    nodes[i].TimeStamp -= shift;

  SmoothTrack();
  UpdateNodeBuffers();
  util::log::Write("Deleted node, remaining nodes %d", nodes.size());
}

void TrackPlayer::UpdateNode(size_t index, Camera const& camera)
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index >= nodes.size()) return;

  CatmullRomNode& node = nodes[index];
  node.FieldOfView = camera.Profile.FieldOfView;
  node.FocusDistance = camera.Profile.FocusDistance;
  node.DofScale = camera.Profile.DofScale;
  node.DofStrength = camera.Profile.DofStrength;
  node.Rotation = camera.Rotation;
  node.Position = camera.Position;

  node.Transform = XMMatrixRotationQuaternion(XMLoadFloat4(&camera.Rotation));
  node.Transform.r[3] = XMLoadFloat3(&camera.Position);
  node.Transform.r[3].m128_f32[3] = 1.0f;

  SmoothTrack();
  UpdateNodeBuffers();
}

void TrackPlayer::SetNodeTime(size_t index, float time)
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index >= nodes.size()) return;

  // The first node stays at 0 and nodes keep their order, at least a tenth of a second apart.
  const float minGap = 0.1f;
  if (index == 0)
    time = 0;
  else
  {
    time = (std::max)(time, nodes[index - 1].TimeStamp + minGap);
    if (index + 1 < nodes.size())
      time = (std::min)(time, nodes[index + 1].TimeStamp - minGap);
  }

  nodes[index].TimeStamp = time;
  SmoothTrack();
  UpdateNodeBuffers();
}

void TrackPlayer::Toggle()
{
  if (m_IsPlaying)
    Stop();
  else
    Play();
}

void TrackPlayer::Play()
{
  if (m_Tracks[m_SelectedTrack].Nodes.size() <= 1)
  {
    util::log::Warning("Can't play a camera track with less than 2 nodes");
    return;
  }

  // Resume from the timeline position unless it's at the end.
  if (m_CurrentTime >= GetDuration())
    m_CurrentTime = 0;
  FindCursors();

  m_IsPlaying = true;
}

void TrackPlayer::Stop()
{
  m_IsPlaying = false;
}

float TrackPlayer::GetDuration()
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  return nodes.empty() ? 0.f : nodes.back().TimeStamp;
}

CatmullRomNode TrackPlayer::NodeState(CatmullRomNode const& node)
{
  CatmullRomNode state;
  state.Rotation = node.Rotation;
  state.Position = node.Position;
  state.FieldOfView = node.FieldOfView;
  state.FocusDistance = node.FocusDistance;
  state.DofScale = node.DofScale;
  state.DofStrength = node.DofStrength;
  state.TimeStamp = node.TimeStamp;
  state.Transform = node.Transform;
  return state;
}

void TrackPlayer::FindCursors()
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  std::vector<SmoothNode> const& smoothNodes = m_Tracks[m_SelectedTrack].SmoothNodes;

  m_CurrentNode = 0;
  while (m_CurrentNode + 2 < nodes.size() && m_CurrentTime >= nodes[m_CurrentNode + 1].TimeStamp)
    m_CurrentNode++;

  m_CurrentSmoothNode = 0;
  while (m_CurrentSmoothNode + 2 < smoothNodes.size() && m_CurrentTime >= smoothNodes[m_CurrentSmoothNode + 1].Time)
    m_CurrentSmoothNode++;
}

void TrackPlayer::Seek(float time)
{
  m_CurrentTime = (std::max)(0.f, (std::min)(time, GetDuration()));
  FindCursors();
}

CatmullRomNode TrackPlayer::Evaluate(float time)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.empty())
    return CatmullRomNode();
  if (nodes.size() == 1)
    return NodeState(nodes[0]);

  const float savedTime = m_CurrentTime;
  const unsigned int savedNode = m_CurrentNode;
  const unsigned int savedSmoothNode = m_CurrentSmoothNode;

  Seek(time);
  CatmullRomNode state = PlayForwardSmooth(0, true);

  m_CurrentTime = savedTime;
  m_CurrentNode = savedNode;
  m_CurrentSmoothNode = savedSmoothNode;
  return state;
}

CatmullRomNode TrackPlayer::PlayForwardSmooth(float dt, bool ignoreManual /*= false*/)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  std::vector<SmoothNode>& smoothNodes = m_Tracks[m_SelectedTrack].SmoothNodes;

  if (nodes.size() < 2 || smoothNodes.size() < 2)
    return nodes.empty() ? CatmullRomNode() : NodeState(nodes[0]);

  if (!m_ManualPlay || ignoreManual)
    m_CurrentTime += dt;
  else
  {
    float controlMultiplier = g_mainHandle->GetInputSystem()->GetActionState(Action::Camera_Up);
    controlMultiplier -= g_mainHandle->GetInputSystem()->GetActionState(Action::Camera_Down);
    m_CurrentTime += dt * controlMultiplier;
  }

  // Past the end: loop, or stop on the last node.
  const float duration = nodes.back().TimeStamp;
  if (m_CurrentTime >= duration)
  {
    if (m_Loop && m_IsPlaying && !ignoreManual)
    {
      m_CurrentTime -= duration;
      if (m_CurrentTime < 0 || m_CurrentTime >= duration)
        m_CurrentTime = 0;
    }
    else
    {
      m_CurrentTime = duration;
      if (m_IsPlaying && !ignoreManual)
        m_IsPlaying = false;
      FindCursors();
      return NodeState(nodes.back());
    }
  }
  if (m_CurrentTime < 0)
    m_CurrentTime = 0;

  if (m_CurrentNode + 1 >= nodes.size())
    m_CurrentNode = static_cast<unsigned int>(nodes.size()) - 2;

  // Move to the node segment containing the current time.
  while (m_CurrentNode + 2 < nodes.size() && m_CurrentTime >= nodes[m_CurrentNode + 1].TimeStamp)
    m_CurrentNode++;
  while (m_CurrentNode > 0 && m_CurrentTime < nodes[m_CurrentNode].TimeStamp)
    m_CurrentNode--;

  std::vector<CatmullRomNode>::iterator n0, n1, n2, n3;
  n1 = nodes.begin() + m_CurrentNode;
  n2 = n1 + 1;
  n0 = m_CurrentNode > 0 ? n1 - 1 : n1;
  n3 = m_CurrentNode + 1 < nodes.size() - 1 ? n2 + 1 : n2;

  // Move to the smooth node containing the current time.
  if (m_CurrentSmoothNode + 1 >= smoothNodes.size())
    m_CurrentSmoothNode = static_cast<unsigned int>(smoothNodes.size()) - 2;
  while (m_CurrentSmoothNode + 2 < smoothNodes.size() && m_CurrentTime >= smoothNodes[m_CurrentSmoothNode + 1].Time)
    m_CurrentSmoothNode++;
  while (m_CurrentSmoothNode > 0 && m_CurrentTime < smoothNodes[m_CurrentSmoothNode].Time)
    m_CurrentSmoothNode--;

  float smoothNodeTimeInterval = smoothNodes[m_CurrentSmoothNode + 1].Time - smoothNodes[m_CurrentSmoothNode].Time;
  float smoothNodeValueInterval = smoothNodes[m_CurrentSmoothNode + 1].Value - smoothNodes[m_CurrentSmoothNode].Value;

  float interpolation = smoothNodeTimeInterval > 0 ? (m_CurrentTime - smoothNodes[m_CurrentSmoothNode].Time) / smoothNodeTimeInterval : 0;
  interpolation = (std::max)(0.f, (std::min)(interpolation, 1.f));
  float mu = (smoothNodes[m_CurrentSmoothNode].Value + smoothNodeValueInterval * interpolation) - m_CurrentNode;
  mu = (std::max)(0.f, (std::min)(mu, 1.f));

  CatmullRomNode resultNode;

  XMVECTOR qRot0 = XMLoadFloat4(&n0->Rotation);
  XMVECTOR qRot1 = XMLoadFloat4(&n1->Rotation);
  XMVECTOR qRot2 = XMLoadFloat4(&n2->Rotation);
  XMVECTOR qRot3 = XMLoadFloat4(&n3->Rotation);

  XMVECTOR vPos0 = XMLoadFloat3(&n0->Position);
  XMVECTOR vPos1 = XMLoadFloat3(&n1->Position);
  XMVECTOR vPos2 = XMLoadFloat3(&n2->Position);
  XMVECTOR vPos3 = XMLoadFloat3(&n3->Position);

  resultNode.FieldOfView = util::math::CatmullRomInterpolate(n0->FieldOfView,
    n1->FieldOfView,
    n2->FieldOfView,
    n3->FieldOfView, mu);

  resultNode.FocusDistance = util::math::CatmullRomInterpolate(n0->FocusDistance,
    n1->FocusDistance,
    n2->FocusDistance,
    n3->FocusDistance, mu);

  resultNode.DofStrength = util::math::CatmullRomInterpolate(n0->DofStrength,
    n1->DofStrength,
    n2->DofStrength,
    n3->DofStrength, mu);

  resultNode.DofScale = util::math::CatmullRomInterpolate(n0->DofScale,
    n1->DofScale,
    n2->DofScale,
    n3->DofScale, mu);

  XMVECTOR resultRot = XMVectorCatmullRom(qRot0, qRot1, qRot2, qRot3, mu);
  XMVECTOR resultPos = XMVectorCatmullRom(vPos0, vPos1, vPos2, vPos3, mu);

  XMStoreFloat4(&resultNode.Rotation, XMQuaternionNormalize(resultRot));
  XMStoreFloat3(&resultNode.Position, resultPos);
  resultNode.TimeStamp = m_CurrentTime;

  return resultNode;
}

CatmullRomNode TrackPlayer::PlayForward(float dt, bool ignoreManual /*= false*/)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;

  if (nodes.size() < 2)
    return nodes.empty() ? CatmullRomNode() : NodeState(nodes[0]);

  // Default time from node to node is 1 second.
  // NodeTimeSpan modifies this.
  float timeMultiplier = (1.f / m_NodeTimeSpan);

  // If manual play is enabled, time is multiplied
  // by input. IgnoreManual is for generating display buffers.
  if (!m_ManualPlay || ignoreManual)
    m_CurrentTime += dt * timeMultiplier;
  else
  {
    float controlMultiplier = g_mainHandle->GetInputSystem()->GetActionState(Action::Camera_Up);
    controlMultiplier -= g_mainHandle->GetInputSystem()->GetActionState(Action::Camera_Down);
    m_CurrentTime += dt * timeMultiplier * controlMultiplier;
  }

  if (m_CurrentTime >= nodes.back().TimeStamp)
  {
    m_CurrentTime = nodes.back().TimeStamp;
    return NodeState(nodes.back());
  }
  if (m_CurrentTime < 0)
    m_CurrentTime = 0;

  if (m_CurrentNode + 1 >= nodes.size())
    m_CurrentNode = static_cast<unsigned int>(nodes.size()) - 2;
  while (m_CurrentNode + 2 < nodes.size() && m_CurrentTime >= nodes[m_CurrentNode + 1].TimeStamp)
    m_CurrentNode++;
  while (m_CurrentNode > 0 && m_CurrentTime < nodes[m_CurrentNode].TimeStamp)
    m_CurrentNode--;

  std::vector<CatmullRomNode>::iterator n0, n1, n2, n3;
  n1 = nodes.begin() + m_CurrentNode;
  n2 = n1 + 1;
  n0 = m_CurrentNode > 0 ? n1 - 1 : n1;
  n3 = m_CurrentNode + 1 < nodes.size() - 1 ? n2 + 1 : n2;

  float segment = n2->TimeStamp - n1->TimeStamp;
  float mu = segment > 0 ? (m_CurrentTime - n1->TimeStamp) / segment : 0;

  CatmullRomNode resultNode;

  XMVECTOR qRot0 = XMLoadFloat4(&n0->Rotation);
  XMVECTOR qRot1 = XMLoadFloat4(&n1->Rotation);
  XMVECTOR qRot2 = XMLoadFloat4(&n2->Rotation);
  XMVECTOR qRot3 = XMLoadFloat4(&n3->Rotation);

  XMVECTOR vPos0 = XMLoadFloat3(&n0->Position);
  XMVECTOR vPos1 = XMLoadFloat3(&n1->Position);
  XMVECTOR vPos2 = XMLoadFloat3(&n2->Position);
  XMVECTOR vPos3 = XMLoadFloat3(&n3->Position);

  resultNode.FieldOfView = util::math::CatmullRomInterpolate(n0->FieldOfView,
    n1->FieldOfView,
    n2->FieldOfView,
    n3->FieldOfView, mu);

  resultNode.FocusDistance = util::math::CatmullRomInterpolate(n0->FocusDistance,
    n1->FocusDistance,
    n2->FocusDistance,
    n3->FocusDistance, mu);

  resultNode.DofStrength = util::math::CatmullRomInterpolate(n0->DofStrength,
    n1->DofStrength,
    n2->DofStrength,
    n3->DofStrength, mu);

  resultNode.DofScale = util::math::CatmullRomInterpolate(n0->DofScale,
    n1->DofScale,
    n2->DofScale,
    n3->DofScale, mu);

  XMVECTOR resultRot = XMVectorCatmullRom(qRot0, qRot1, qRot2, qRot3, mu);
  XMVECTOR resultPos = XMVectorCatmullRom(vPos0, vPos1, vPos2, vPos3, mu);

  XMStoreFloat4(&resultNode.Rotation, XMQuaternionNormalize(resultRot));
  XMStoreFloat3(&resultNode.Position, resultPos);
  resultNode.TimeStamp = m_CurrentTime;

  return resultNode;
}

void TrackPlayer::DrawUI()
{
  ImGui::Text("Camera tracks");
  ImGui::Combo("##CameraTrackList", (int*)&m_SelectedTrack, &m_TrackNames[0], (int)m_TrackNames.size());
  if (ImGui::Button("Create", ImVec2(95, 25)))
    CreateTrack();
  ImGui::SameLine(0, 10);
  if (ImGui::Button("Delete", ImVec2(95, 25)))
    DeleteTrack();
  ImGui::Dummy(ImVec2(0, 5));
  ImGui::Dummy(ImVec2(0, 5));
  ImGui::Text("Time between previous node");
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 10));
  if (ImGui::InputFloat("##CameraTrackNodeTime", &m_NodeTimeSpan, 0.1f, 0, 2))
    m_NodeTimeSpan = (std::max)(0.1f, m_NodeTimeSpan);

  ImGui::Checkbox("Lock field of view", &m_LockFieldOfView);
  ImGui::Checkbox("Lock depth of field", &m_LockDepthOfField);
  ImGui::Checkbox("Lock rotation", &m_LockRotation);
  ImGui::Checkbox("Play manually", &m_ManualPlay);
  ImGui::Checkbox("Loop", &m_Loop);
  ImGui::Checkbox("Show track in world", &m_ShowTrack);
  ImGui::PopStyleVar();
}

void TrackPlayer::DrawNodes()
{
  if (m_IsPlaying) return;

  CameraTrack& track = m_Tracks[m_SelectedTrack];
  for (auto& node : track.Nodes)
    g_mainHandle->GetRenderer()->DrawModel(m_pCameraModel.get(), node.Transform, { 1,0,0 });

  if (track.IndexCount > 0)
    g_mainHandle->GetRenderer()->DrawLines(track.Indices.Get(), track.Vertices.Get(), track.IndexCount);
}

void TrackPlayer::CreateTrack()
{
  if (m_IsPlaying) return;

  m_Tracks.emplace_back("Track #" + std::to_string(m_RunningId++));
  m_SelectedTrack = static_cast<unsigned int>(m_Tracks.size()) - 1;
  m_CurrentTime = 0;
  m_CurrentNode = 0;
  m_CurrentSmoothNode = 0;

  UpdateNameList();
}

void TrackPlayer::DeleteTrack()
{
  if (m_IsPlaying || m_Tracks.size() <= 1) return;

  m_Tracks.erase(m_Tracks.begin() + m_SelectedTrack);
  if (m_SelectedTrack >= m_Tracks.size())
    m_SelectedTrack -= 1;
  m_CurrentTime = 0;
  m_CurrentNode = 0;
  m_CurrentSmoothNode = 0;

  UpdateNameList();
}

void TrackPlayer::UpdateNodeBuffers()
{
  CameraTrack& track = m_Tracks[m_SelectedTrack];
  const std::vector<CatmullRomNode>& nodes = track.Nodes;

  track.Vertices.Reset();
  track.Indices.Reset();
  track.IndexCount = 0;

  if (nodes.size() < 2)
  {
    m_CurrentTime = 0;
    m_CurrentNode = 0;
    m_CurrentSmoothNode = 0;
    return;
  }

  std::vector<VertexPositionColor> verticesVector;
  std::vector<unsigned int> indicesVector;

  VertexPositionColor v1, v2;
  v1.position = nodes[0].Position;
  v1.color = v2.color = XMFLOAT4(1, 0, 0, 1);

  XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&nodes[0].Rotation));
  XMStoreFloat3(&v2.position, XMLoadFloat3(&nodes[0].Position) - 0.5f*forward);

  verticesVector.push_back(v2);
  verticesVector.push_back(v1);
  indicesVector.push_back(0);
  indicesVector.push_back(1);

  m_CurrentTime = 0;
  m_CurrentNode = 0;
  int stepCount = 0;

  // Play through the track, save nodes into vertices
  while (m_CurrentTime < nodes[nodes.size() - 1].TimeStamp)
  {
    stepCount++;

    VertexPositionColor nodeVertex, forwardVertex;
    nodeVertex.color = forwardVertex.color = XMFLOAT4(1, 0, 0, 1);

    CatmullRomNode node = PlayForward(0.1f, true);
    nodeVertex.position = node.Position;

    verticesVector.push_back(nodeVertex);
    indicesVector.push_back((unsigned int)verticesVector.size() - 1); // Current node vertex

    // Create forward line to indicate 0.5 second of track time
    if (stepCount % 5 == 0)
    {
      XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&node.Rotation));
      XMStoreFloat3(&forwardVertex.position, XMLoadFloat3(&node.Position) - 0.5*forward);

      verticesVector.push_back(forwardVertex);
      indicesVector.push_back((unsigned int)verticesVector.size() - 1);
      indicesVector.push_back((unsigned int)verticesVector.size() - 2);
    }
  }

  track.IndexCount = (unsigned int)indicesVector.size();
  g_mainHandle->GetRenderer()->CreateVertexIndexBuffers(indicesVector, verticesVector, track.Vertices.GetAddressOf(), track.Indices.GetAddressOf());

  m_CurrentTime = 0;
  m_CurrentNode = 0;
  m_CurrentSmoothNode = 0;
}

void TrackPlayer::UpdateNameList()
{
  m_TrackNames.clear();
  for (auto& track : m_Tracks)
    m_TrackNames.push_back(track.Name.c_str());
}

// Generates interpolation values for irregular time intervals
void TrackPlayer::SmoothTrack()
{
  const std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  std::vector<SmoothNode>& smoothNodes = m_Tracks[m_SelectedTrack].SmoothNodes;

  smoothNodes.clear();
  if (nodes.size() < 2) return;

  const int stepsPerNode = 100;
  const int lastNode = static_cast<int>(nodes.size()) - 1;

  // Step through the track in hundredths of a node, so the sampled time never drifts past a node
  // through accumulated rounding, and finish exactly on the last node.
  for (int step = 0; step <= lastNode * stepsPerNode; ++step)
  {
    float currentStep = step / static_cast<float>(stepsPerNode);
    int currentNode = (std::min)(step / stepsPerNode, lastNode - 1);

    int n1 = currentNode;
    int n2 = currentNode + 1;
    int n0 = n1 > 0 ? n1 - 1 : n1;
    int n3 = n2 < lastNode ? n2 + 1 : n2;

    SmoothNode smoothNode;
    smoothNode.Time = util::math::CatmullRomInterpolate(nodes[n0].TimeStamp, nodes[n1].TimeStamp, nodes[n2].TimeStamp, nodes[n3].TimeStamp, currentStep - n1);
    smoothNode.Value = currentStep;

    // Interpolated time has to keep increasing for the playback search to work.
    if (!smoothNodes.empty() && smoothNode.Time < smoothNodes.back().Time)
      smoothNode.Time = smoothNodes.back().Time;

    smoothNodes.push_back(smoothNode);
  }

  smoothNodes.back().Time = nodes[lastNode].TimeStamp;
  smoothNodes.back().Value = static_cast<float>(lastNode);

  util::log::Write("Created %d smooth nodes", smoothNodes.size());
}
